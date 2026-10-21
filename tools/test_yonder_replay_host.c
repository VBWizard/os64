/* THE REPLAY RUNNER (MODERN_WEB.md rule 3, § Slice 0b, as built): a page
 * yonder kept beside its page file (yonder.conf's keep_pages, diag.h) fed
 * back to yonder's own window on the host, with no network, and its record
 * written again.
 *
 *     test_yonder_replay_host KEPT-DIR OUT-DIR [--seconds N] [--only TEXT]
 *     test_yonder_replay_host --self-test WORK-DIR
 *
 * The window and every fake are tools/test_yonder_scripts_host.c's, the
 * headless binding: this file includes it for them and brings its own main.
 * For each `<name>.txt` in KEPT-DIR that has a `<name>.html`:
 *
 *   - a window as wide as the record says the page was laid out, scripts
 *     on unless the record says `scripts: off`, the script time limit the
 *     default (the record does not say what it was), and a text context of
 *     the page's own;
 *   - start_trip to the address on the record's first line. A page from the
 *     network gets a head rebuilt from the record's facts (status, final
 *     address, body, content-type, charset, encrypted, TLS), its bytes
 *     posted in the stream's chunk size, and the fetch's verdict; a file's
 *     page is read from the kept bytes by the window's own open_local;
 *   - the loop turned as yonder's main loop turns it (the stream's turn,
 *     the waiting page's, the shown page's, the pictures' settle, the tick
 *     that shows a page whose sheets are late), with the jobs the window
 *     submitted answered at the top of each turn: a script or a sheet from
 *     `<name>.d/index` by its exact address, as it came or as the failure
 *     it was where the page was kept. One the index has no line for is not
 *     made up either way: it stays out, as if still on the wire, and the
 *     record gains `REPLAY incomplete <kind> <url>` (rp_answer). Pictures
 *     and fonts are never answered: they are not kept, and a page shows
 *     without them, as it does while they come;
 *   - THE CLOCK. A turn with nothing to do moves the window's milliseconds
 *     on by a tick (10 ms), until the page is quiet (nothing arriving,
 *     nothing out that will be answered, no timer set) or the virtual
 *     budget is spent (60 s, --seconds), so a page's timers fire as they
 *     would on the glass. The microsecond clock also moves 1 ms each time
 *     it is read, so a script that never ends is stopped at the script
 *     time limit as it is on the P5;
 *   - the page left as yonder leaves it when it closes, which is the
 *     departure write: that record, and its REPLAY lines after it, goes to
 *     OUT-DIR under the same name, and one line is printed, `incomplete`
 *     when it has REPLAY lines, else `clean` or the verdict's counts, with
 *     what went wrong in the replay itself beside it (the page never
 *     arrived, it asked for the next turn for ever, it did not let go of
 *     memory). A summary closes: `replayed P, clean C, incomplete I`, and
 *     how many records had no kept bytes. A record that says `kept: not
 *     whole`, or whose index names a format other than the one this runner
 *     reads, is counted and not replayed.
 *
 * --self-test is the proof (run by tools/test_yonder_replay_host.sh): the
 * fixture pages the scripted-page host's fixtures serve
 * (tools/yonder_loop_fixture, tools/yonder_script_fixture, tools/pages) and
 * hand-made ones are loaded with keep_pages on, as a person browsing would,
 * their sub-resources served by the runner; what yonder kept is written out
 * to WORK-DIR/kept beside the records it wrote, replayed into WORK-DIR/out
 * by the path above, and each replayed record must be the page's own line
 * for line, its paint stages aside (rp_same). Then one of them is replayed
 * with a line taken out of its index, and must read as incomplete
 * (rp_incomplete_case).
 */
#define YONDER_HOST_FAKES_ONLY
#pragma GCC diagnostic ignored "-Wunused-function"
#include "test_yonder_scripts_host.c"

#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

/* How far the clocks move: a tick when the loop has nothing to do, and the
 * microsecond clock at each read. */
#define RP_TICK_MS 10
#define RP_MICROS_STEP 1000
/* Turns in a row that each asked to come straight back: past this the page
 * is said to ring itself for ever, and is left. */
#define RP_BUSY_MAX 100000

/* ── Files on the host ──────────────────────────────────────────────── */

/* A whole file, NUL-terminated past its length; NULL when it cannot be read. */
static char *rp_slurp(const char *path, size_t *len) {
    FILE *f=fopen(path,"rb");
    if(f==NULL) return NULL;
    char *data=NULL; size_t n=0, cap=0;
    for(;;) {
        if(n+65536+1>cap) {
            cap=cap ? cap*2 : 131072;
            char *grown=realloc(data,cap);
            if(grown==NULL) { free(data); fclose(f); return NULL; }
            data=grown;
        }
        size_t got=fread(data+n,1,65536,f);
        n+=got;
        if(got==0) break;
    }
    bool bad=ferror(f); fclose(f);
    if(bad) { free(data); return NULL; }
    data[n]='\0';
    if(len!=NULL) *len=n;
    return data;
}
static bool rp_write(const char *path, const void *bytes, size_t len) {
    FILE *f=fopen(path,"wb");
    if(f==NULL) return false;
    bool ok=fwrite(bytes,1,len,f)==len;
    return fclose(f)==0 && ok;
}
static bool rp_exists(const char *path) { struct stat st; return stat(path,&st)==0; }
/* `a`, `b` and `c` end to end: a path, malloc'd, however long. */
static char *rp_cat(const char *a, const char *b, const char *c) {
    size_t n=strlen(a)+strlen(b)+strlen(c)+1;
    char *s=malloc(n); snprintf(s,n,"%s%s%s",a,b,c);
    return s;
}
static int rp_strcmp(const void *a, const void *b) { return strcmp(*(char *const *)a,*(char *const *)b); }

/* A field as a page file or an index wrote it (diag.c): `\\` and `\xHH`
 * read back, and, with `tokens`, a token whose second letter was
 * percent-encoded. A new string. */
static char *rp_unescape(const char *s, size_t n, bool tokens) {
    char *out=malloc(n+1); size_t o=0;
    for(size_t i=0;i<n;) {
        if(s[i]=='\\' && i+1<n && s[i+1]=='\\') { out[o++]='\\'; i+=2; continue; }
        if(s[i]=='\\' && i+3<n && s[i+1]=='x' && isxdigit((unsigned char)s[i+2]) && isxdigit((unsigned char)s[i+3])) {
            char hex[3]={s[i+2],s[i+3],0}; out[o++]=(char)strtol(hex,NULL,16); i+=4; continue;
        }
        if(tokens && i+9<=n && !memcmp(s+i,"M%49SSING",9)) { memcpy(out+o,"MISSING",7); o+=7; i+=9; continue; }
        if(tokens && i+8<=n && !memcmp(s+i,"F%41ILED",8)) { memcpy(out+o,"FAILED",6); o+=6; i+=8; continue; }
        out[o++]=s[i++];
    }
    out[o]='\0';
    return out;
}

/* ── What the record says ───────────────────────────────────────────── */

enum { F_STATUS, F_FINAL, F_BODY, F_TYPE, F_CHARSET, F_ENCRYPTED, F_TLS, F_SCRIPTS, F_LAID, F_KEPT, F_N };
static const char *const kFacts[F_N]={"status","final address","body","content-type","charset","encrypted",
    "TLS","scripts","laid out at","kept"};
typedef struct {
    char *address;          /* line one, decoded */
    char *verdict;          /* line two, as written */
    char *facts[F_N];       /* by kFacts, decoded; NULL when absent */
} RpRecord;

static void rp_record_free(RpRecord *r) {
    free(r->address); free(r->verdict);
    for(int i=0;i<F_N;i++) free(r->facts[i]);
    memset(r,0,sizeof(*r));
}
static bool rp_record_read(const char *text, RpRecord *r) {
    memset(r,0,sizeof(*r));
    if(strncmp(text,"address: ",9)!=0) return false;
    for(const char *line=text; *line; ) {
        const char *end=strchr(line,'\n'); size_t len=end ? (size_t)(end-line) : strlen(line);
        if(line==text) r->address=rp_unescape(line+9,len-9,true);
        else if(r->verdict==NULL) r->verdict=rp_unescape(line,len,false);
        else for(int i=0;i<F_N;i++) {
            size_t k=strlen(kFacts[i]);
            if(r->facts[i]==NULL && len>k+1 && !strncmp(line,kFacts[i],k) && line[k]==':' && line[k+1]==' ')
                r->facts[i]=rp_unescape(line+k+2,len-k-2,true);
        }
        line+=len+(end ? 1 : 0);
    }
    return r->address!=NULL && r->verdict!=NULL;
}

/* ── What the window's jobs are answered with ───────────────────────── */

typedef struct {
    const uint8_t *bytes; size_t len;
    const char *final, *label;
} RpAnswer;
/* What a source of answers knows of an address: it came (`out` filled),
 * its fetch failed where the page was kept, or nothing at all, which a
 * replay cannot make up: the job is left out, as if still on the wire,
 * and the record says the replay was incomplete. */
typedef enum { RP_CAME, RP_FAILED, RP_UNKNOWN } RpKnown;
typedef RpKnown (*rp_lookup_t)(void *ctx, uint32_t kind, const char *url, RpAnswer *out);

/* The index of a kept page's folder (diag.h): its lines of the kinds this
 * runner answers, each file read when first asked for, and how often each
 * was handed out, so the second job for one address is given the second
 * line kept for it, and a job past the last is given the first again. A
 * kind it does not know is passed over: a later slice's lines answer that
 * slice's requests. */
typedef struct { uint32_t kind; bool came; char *n, *url, *final, *label; char *bytes; size_t len; bool read; unsigned used; } RpEntry;
typedef struct { char *folder; RpEntry *e; size_t n; bool versioned; } RpIndex;

static void rp_index_free(RpIndex *x) {
    for(size_t i=0;i<x->n;i++) { free(x->e[i].n); free(x->e[i].url); free(x->e[i].final); free(x->e[i].label); free(x->e[i].bytes); }
    free(x->e); free(x->folder); memset(x,0,sizeof(*x));
}
/* The folder's index; `versioned` false when there is one whose first line
 * names a format this runner does not read. A page with no folder fetched
 * nothing it kept, and its index is empty. */
static void rp_index_read(const char *folder, RpIndex *x) {
    memset(x,0,sizeof(*x));
    x->folder=strdup(folder);
    x->versioned=true;
    char *path=rp_cat(folder,"/index","");
    char *text=rp_slurp(path,NULL);
    free(path);
    if(text==NULL) return;
    char head[64]; snprintf(head,sizeof(head),"# yonder kept index %d\n",YONDER_DIAG_INDEX_VERSION);
    x->versioned=!strncmp(text,head,strlen(head));
    for(char *line=text; x->versioned && *line; ) {
        char *end=strchr(line,'\n'); if(end) *end='\0';
        char *f[6]={0}; int nf=0;
        for(char *p=line; nf<6; ) { f[nf++]=p; char *t=strchr(p,'\t'); if(!t) break; *t='\0'; p=t+1; }
        if(line[0]!='#' && nf==6 && (!strcmp(f[1],"script") || !strcmp(f[1],"sheet"))) {
            x->e=realloc(x->e,(x->n+1)*sizeof(*x->e));
            RpEntry *e=&x->e[x->n++]; memset(e,0,sizeof(*e));
            e->kind=!strcmp(f[1],"script") ? YONDER_JOB_SCRIPT : YONDER_JOB_SHEET;
            e->n=strdup(f[0]);
            e->url=rp_unescape(f[2],strlen(f[2]),false);
            e->came=!strcmp(f[3],"came");
            e->final=rp_unescape(f[4],strlen(f[4]),false);
            e->label=rp_unescape(f[5],strlen(f[5]),false);
        }
        if(!end) break;
        line=end+1;
    }
    free(text);
}
static RpKnown rp_index_lookup(void *ctx, uint32_t kind, const char *url, RpAnswer *out) {
    RpIndex *x=ctx; RpEntry *first=NULL, *fresh=NULL;
    for(size_t i=0;i<x->n;i++)
        if(x->e[i].kind==kind && !strcmp(x->e[i].url,url)) {
            if(first==NULL) first=&x->e[i];
            if(fresh==NULL && x->e[i].used==0) fresh=&x->e[i];
        }
    RpEntry *pick=fresh!=NULL ? fresh : first;
    if(pick==NULL) return RP_UNKNOWN;
    pick->used++;
    if(!pick->came) return RP_FAILED;
    if(!pick->read) {
        char *file=rp_cat(pick->n,".",kind==YONDER_JOB_SCRIPT ? "js" : "css");
        char *path=rp_cat(x->folder,"/",file);
        pick->bytes=rp_slurp(path,&pick->len); pick->read=true;
        free(path); free(file);
    }
    /* A line whose file is gone is a kept page with a piece missing. */
    if(pick->bytes==NULL) return RP_UNKNOWN;
    *out=(RpAnswer){(const uint8_t *)pick->bytes,pick->len,pick->final,pick->label};
    return RP_CAME;
}

/* ── The pool, run here ─────────────────────────────────────────────── */

/* Every job the window submits, in the order it did; `unknown` once its
 * source had nothing for it, which leaves it out for good. */
typedef struct { os64_work_id_t id; os64_work_t work; uint32_t kind; bool unknown; } RpJob;
static RpJob *rp_jobs; static size_t rp_njobs, rp_capjobs;
static os64_work_id_t rp_ids;
static rp_lookup_t rp_lookup; static void *rp_lookup_ctx;
/* What the page asked for that its source had nothing for, as the lines
 * `REPLAY incomplete <kind> <url>` the replayed record gains. */
static char *rp_incomplete; static size_t rp_incomplete_len;

static os64_work_id_t rp_submit(const os64_work_t *work) {
    if(rp_njobs==rp_capjobs) { rp_capjobs=rp_capjobs ? rp_capjobs*2 : 64; rp_jobs=realloc(rp_jobs,rp_capjobs*sizeof(*rp_jobs)); }
    rp_jobs[rp_njobs]=(RpJob){++rp_ids,*work,*(const uint32_t *)work->job,false};
    return rp_jobs[rp_njobs++].id;
}
/* A field of a REPLAY line, written as a page file writes data (diag.c):
 * a byte that would break the line, and a backslash, escaped, and a token
 * with its second letter percent-encoded, so the line answers no grep for
 * MISSING or FAILED. */
static void rp_incomplete_add(const char *kind, const char *url) {
    size_t n=strlen(kind)+strlen(url)*4+32;
    rp_incomplete=realloc(rp_incomplete,rp_incomplete_len+n);
    char *o=rp_incomplete+rp_incomplete_len;
    o+=sprintf(o,"REPLAY incomplete %s ",kind);
    for(const char *s=url; *s; s++) {
        unsigned char c=(unsigned char)*s;
        if(!strncmp(s,"MISSING",7) || !strncmp(s,"FAILED",6)) { o+=sprintf(o,"%c%%%02X",s[0],(unsigned char)s[1]); s++; }
        else if(c=='\\') o+=sprintf(o,"\\\\");
        else if(c<0x20 || c==0x7F) o+=sprintf(o,"\\x%02X",c);
        else *o++=(char)c;
    }
    *o++='\n';
    rp_incomplete_len=(size_t)(o-rp_incomplete);
}
static void rp_take(size_t i) { memmove(rp_jobs+i,rp_jobs+i+1,(rp_njobs-i-1)*sizeof(*rp_jobs)); rp_njobs--; }
/* A cancelled job is the pool's to release (work.h). */
static bool rp_cancel(os64_work_id_t id) {
    for(size_t i=0;i<rp_njobs;i++) if(rp_jobs[i].id==id) {
        RpJob j=rp_jobs[i]; rp_take(i);
        j.work.release(j.work.job,NULL);
        return true;
    }
    return false;
}
/* The kinds the replay answers; pictures and fonts it never does. */
static bool rp_answers(const RpJob *j) {
    return !j->unknown && (j->kind==YONDER_JOB_TRIP || j->kind==YONDER_JOB_SCRIPT || j->kind==YONDER_JOB_SHEET);
}

/* A script's product as script_job.c makes it: the source as text, or a
 * fetch that failed. */
static void *rp_script_product(const yonder_script_job_t *job, RpKnown known, const RpAnswer *a) {
    yonder_script_t *got=os64_calloc(1,sizeof(*got));
    os64_strcopy(got->url,sizeof(got->url),job->url);
    if(known==RP_CAME) {
        got->ok=true; got->length=a->len;
        got->source=os64_malloc(a->len+1); memcpy(got->source,a->bytes,a->len); got->source[a->len]='\0';
        os64_strcopy(got->url,sizeof(got->url),a->final);
    }
    return got;
}
/* A sheet's product as sheet.c makes it: parsed under its response's label
 * and the document's encoding, carrying its bytes back when the window
 * keeps its pages; or a fetch that failed. */
static void *rp_sheet_product(const yonder_sheet_job_t *job, RpKnown known, const RpAnswer *a) {
    yonder_sheet_t *got=os64_calloc(1,sizeof(*got));
    os64_strcopy(got->url,sizeof(got->url),job->url);
    if(known==RP_CAME) {
        got->ok=garb_parse_sheet(a->bytes,a->len,a->label[0] ? a->label : NULL,
            job->environment[0] ? job->environment : NULL,&got->parsed)==GARB_OK;
        os64_strcopy(got->url,sizeof(got->url),a->final);
        os64_strcopy(got->label,sizeof(got->label),a->label);
        if(got->ok && job->keep) {
            got->kept=os64_malloc(a->len ? a->len : 1); memcpy(got->kept,a->bytes,a->len); got->kept_len=a->len;
        }
    }
    return got;
}
/* One round trip of the network: every job out when it began that the
 * replay answers, answered in the order it was submitted. A job submitted
 * while answering waits for the next. A navigation's job comes back with
 * nothing: everything it had to say went down the mailbox. A script or a
 * sheet the source knows nothing of is not made up, neither as content
 * nor as a failure: it stays out, and the record says the replay was
 * incomplete (MODERN_WEB_BRIEFS.md § Slice 0b), so a page that asks for
 * something new reads as a corpus to record again, not as a regression. */
static void rp_answer(void) {
    os64_work_id_t last=rp_ids;
    for(;;) {
        size_t i=0;
        while(i<rp_njobs && !(rp_jobs[i].id<=last && rp_answers(&rp_jobs[i]))) i++;
        if(i==rp_njobs) return;
        RpJob *j=&rp_jobs[i];
        void *product=NULL;
        if(j->kind!=YONDER_JOB_TRIP) {
            const char *url=j->kind==YONDER_JOB_SCRIPT ? ((const yonder_script_job_t *)j->work.job)->url
                                                       : ((const yonder_sheet_job_t *)j->work.job)->url;
            RpAnswer a;
            RpKnown known=rp_lookup(rp_lookup_ctx,j->kind,url,&a);
            if(known==RP_UNKNOWN) {
                j->unknown=true;
                rp_incomplete_add(j->kind==YONDER_JOB_SCRIPT ? "script" : "sheet",url);
                continue;
            }
            product=j->kind==YONDER_JOB_SCRIPT ? rp_script_product(j->work.job,known,&a)
                                               : rp_sheet_product(j->work.job,known,&a);
        }
        RpJob taken=*j; rp_take(i);
        reaped(taken.id,taken.work.job,product);
    }
}
static bool rp_answerable(void) {
    for(size_t i=0;i<rp_njobs;i++) if(rp_answers(&rp_jobs[i])) return true;
    return false;
}
/* What is never answered (pictures, fonts, what the source knew nothing
 * of), let go as the pool lets go a job it was destroyed holding. */
static size_t rp_release_all(void) {
    size_t n=rp_njobs;
    while(rp_njobs>0) { RpJob j=rp_jobs[rp_njobs-1]; rp_njobs--; j.work.release(j.work.job,NULL); }
    return n;
}

/* ── One page ───────────────────────────────────────────────────────── */

typedef struct {
    const char *address;            /* as asked for: the record's first line */
    uint32_t seq;                   /* the record's own, so the file keeps its name */
    bool local;                     /* a file's page: read by open_local */
    way_head_t head;                /* a network page's */
    bool scripts;
    int32_t width;                  /* the view's, 0 for the harness's own */
    const uint8_t *bytes; size_t len;
    const char *dir;                /* where on the fake disk the window writes */
    bool keep;                      /* keep_pages, as a person browsing has it */
    uint64_t budget_ms;
} RpLoad;
typedef enum { RP_QUIET, RP_SPENT, RP_BUSY } RpEnd;
typedef struct { RpEnd end; bool arrived; unsigned harness_failures; size_t live_after; } RpRan;

static bool rp_quiet(void) {
    if(g.stream.active || g.coming.active || rp_answerable()) return false;
    return !yonder_scripts_alive(g.page.scripts) || yonder_scripts_timer_next(g.page.scripts)==UINT64_MAX;
}
/* The loop turned as main turns it, the clock moved a tick at a time when
 * a turn finds nothing to do. */
static RpEnd rp_settle(uint64_t budget_ms) {
    uint64_t waited=0; unsigned busy=0;
    for(;;) {
        rp_answer();
        bool more=stream_turn();
        more=coming_turn() || more;
        more=script_turn() || more;
        pictures_settle();
        if(g.coming.active && yonder_now_ms()>=g.coming.due) { coming_show(); more=true; }
        if(more) {
            if(++busy>=RP_BUSY_MAX) return RP_BUSY;
            continue;
        }
        busy=0;
        if(rp_quiet()) return RP_QUIET;
        if(waited>=budget_ms) return RP_SPENT;
        now_ms+=RP_TICK_MS; clock_us+=RP_TICK_MS*1000; waited+=RP_TICK_MS;
    }
}
/* A window as the record's page had, the page delivered, the loop settled,
 * the page left. The departure write is on the fake disk, in load->dir. */
static RpRan rp_run(const RpLoad *load, rp_lookup_t lookup, void *ctx) {
    RpRan ran={0};
    unsigned failed=failures;
    rp_lookup=lookup; rp_lookup_ctx=ctx;
    rp_incomplete_len=0;
    /* A text context of the page's own: what one page's glyphs left in its
     * caches is neither a later page's to find nor held against it. */
    os64_text_options_t options={.memory={.alloc=probe_alloc,.free=probe_free},
        .backend=flow_test_backend(),.memory_cap=8*1024*1024};
    check(os64_text_create(&options,&probe_text)==OS64_FONT_OK,"replay: a text context");
    check(os64_text_font_bitmap(probe_text,&probe_font)==OS64_FONT_OK,"replay: its bitmap face");
    stream_window();
    pool_submit_hook=rp_submit; pool_cancel_hook=rp_cancel;
    g.scripts_on=load->scripts; g.script_ms=(uint64_t)YONDER_SCRIPT_SECONDS_DEFAULT*1000;
    layout();
    if(load->width>0) {
        g.root.bounds.w+=load->width-g.view.bounds.w;
        layout();
    }
    memset(&s_diag,0,sizeof(s_diag));
    os64_strcopy(s_diag.dir,sizeof(s_diag.dir),load->dir);
    s_diag.next=load->seq; s_diag.keep=load->keep;
    now_ms=100; clock_us=10000000; clock_step=RP_MICROS_STEP;
    if(load->local) {
        slurp_path=load->address+strlen("file://"); slurp_bytes=(const char *)load->bytes; slurp_length=load->len;
        if(load->len==0) slurp_bytes="";
        start_trip(load->address,NULL,NAV_GO,NULL);
        slurp_path=NULL; slurp_bytes=NULL; slurp_length=0;
    } else {
        start_trip(load->address,NULL,NAV_GO,NULL);
        if(g.stream.active && g.stream.mail!=NULL) {
            yonder_mail_post_head(g.stream.mail,&load->head);
            for(size_t at=0;at<load->len;at+=YONDER_STREAM_CHUNK)
                yonder_mail_post(g.stream.mail,load->bytes+at,
                    load->len-at<YONDER_STREAM_CHUNK ? load->len-at : YONDER_STREAM_CHUNK,NULL,NULL);
            yonder_verdict_t v; memset(&v,0,sizeof(v)); v.page=true; v.fetch=OS64_FETCH_OK;
            yonder_mail_post_verdict(g.stream.mail,&v);
        }
    }
    ran.end=rp_settle(load->budget_ms);
    ran.arrived=page_doc(&g.page)!=NULL;
    /* Leaving, in yonder's own order as it closes: the pool first, which
     * lets go of what it still holds, then the pages, which write their
     * records. */
    clock_step=0;
    rp_release_all();
    pool_submit_hook=NULL; pool_cancel_hook=NULL; pool_open=false; g.pool=NULL;
    if(g.stream.active) stream_drop();
    inputs_drop();
    yonder_scripts_free(g.page.scripts); g.page.scripts=NULL;
    forms_drop();
    page_clear(&g.page); page_clear(&g.coming.page);
    os64_ui_font_release(&g.ui);
    os64_text_font_release(probe_font); probe_font=NULL;
    check(os64_text_destroy(probe_text)==OS64_FONT_OK,"replay: the page's text is all let go");
    probe_text=NULL;
    ran.harness_failures=failures-failed;
    ran.live_after=live;
    return ran;
}

/* The record a run wrote on the fake disk: its file under `dir`. */
static const char *rp_written(const char *dir, const char *address, uint32_t seq, char *name, size_t cap, size_t *len) {
    yonder_diag_t *d=yonder_diag_new(address,seq,0);
    bool named=d!=NULL && yonder_diag_file_name(d,name,cap);
    yonder_diag_free(d);
    if(!named) return NULL;
    char *path=rp_cat(dir,"/",name);
    const char *bytes=fake_bytes(path,len);
    free(path);
    return bytes;
}

/* ── Replaying a directory ──────────────────────────────────────────── */

typedef struct { unsigned replayed, clean, incomplete, bare, not_whole, unarrived, busy, leaked, broken; } RpTally;

/* The record's sequence, from its file name `<anything>-<digits>.txt`. */
static uint32_t rp_seq(const char *name) {
    size_t n=strlen(name);
    if(n<6 || strcmp(name+n-4,".txt")) return 0;
    size_t at=n-4; while(at>0 && isdigit((unsigned char)name[at-1])) at--;
    return at<n-4 && at>0 && name[at-1]=='-' ? (uint32_t)strtoul(name+at,NULL,10) : 0;
}

/* One kept page replayed into `out`; its line printed. */
static void rp_replay_one(const char *kept, const char *out, const char *name, uint64_t budget_ms, RpTally *t) {
    char *stem=strndup(name,strlen(name)-4);
    char *base=rp_cat(kept,"/",stem);
    free(stem);
    char *path=rp_cat(base,".txt","");
    char *text=rp_slurp(path,NULL);
    free(path);
    RpRecord r;
    if(text==NULL || !rp_record_read(text,&r)) {
        free(text); free(base);
        return;                     /* not a page file */
    }
    free(text);
    path=rp_cat(base,".html","");
    size_t len=0;
    char *bytes=rp_exists(path) ? rp_slurp(path,&len) : NULL;
    free(path);
    if(bytes==NULL) { t->bare++; rp_record_free(&r); free(base); return; }
    if(r.facts[F_KEPT]!=NULL) {
        printf("%s  not replayed: %s\n",name,r.facts[F_KEPT]);
        t->not_whole++; free(bytes); rp_record_free(&r); free(base); return;
    }
    RpLoad load={0};
    load.address=r.address; load.seq=rp_seq(name); load.bytes=(const uint8_t *)bytes; load.len=len;
    load.local=!strncmp(r.address,"file://",7);
    load.scripts=r.facts[F_SCRIPTS]==NULL || strcmp(r.facts[F_SCRIPTS],"off")!=0;
    load.width=r.facts[F_LAID]!=NULL ? atoi(r.facts[F_LAID]) : 0;
    load.dir="/tmp/replay"; load.budget_ms=budget_ms;
    way_head_t *h=&load.head;
    if(r.facts[F_STATUS]!=NULL) {
        h->status=atoi(r.facts[F_STATUS]);
        const char *sp=strchr(r.facts[F_STATUS],' ');
        os64_strcopy(h->reason,sizeof(h->reason),sp!=NULL ? sp+1 : "");
    } else {
        h->status=200; os64_strcopy(h->reason,sizeof(h->reason),"OK");
    }
    os64_strcopy(h->url,sizeof(h->url),r.facts[F_FINAL]!=NULL ? r.facts[F_FINAL] : r.address);
    os64_strcopy(h->content_type,sizeof(h->content_type),r.facts[F_TYPE]!=NULL ? r.facts[F_TYPE] : "");
    os64_strcopy(h->charset,sizeof(h->charset),r.facts[F_CHARSET]!=NULL ? r.facts[F_CHARSET] : "");
    h->encrypted=r.facts[F_ENCRYPTED]!=NULL && !strcmp(r.facts[F_ENCRYPTED],"yes");
    const char *body=r.facts[F_BODY]!=NULL ? r.facts[F_BODY] : "html";
    h->body=!strcmp(body,"text") ? WAY_BODY_TEXT : !strcmp(body,"image") ? WAY_BODY_IMAGE : WAY_BODY_HTML;
    if(r.facts[F_TLS]!=NULL) {
        h->tls_version=!strncmp(r.facts[F_TLS],"1.3",3) ? 0x0304 : 0x0303;
        h->tls_fallback=strstr(r.facts[F_TLS],"fallback")!=NULL;
    }
    RpIndex index;
    path=rp_cat(base,".d",""); rp_index_read(path,&index); free(path);
    free(base);
    if(!index.versioned) {
        printf("%s  not replayed: its index is not in the format this runner reads (# yonder kept index %d)\n",
               name,YONDER_DIAG_INDEX_VERSION);
        t->not_whole++; rp_index_free(&index); free(bytes); rp_record_free(&r); return;
    }
    fake_reset(); snprintf(fake_dirs[nfake_dirs++],256,"/tmp/replay");
    size_t live_before=live;
    RpRan ran=rp_run(&load,rp_index_lookup,&index);
    char written[256]; size_t wlen=0;
    const char *record=rp_written("/tmp/replay",r.address,load.seq,written,sizeof(written),&wlen);
    t->replayed++;
    /* The record as the window wrote it, then what the replay could not
     * answer. */
    path=rp_cat(out,"/",name);
    char *whole=NULL;
    if(record!=NULL) {
        whole=malloc(wlen+rp_incomplete_len+1);
        memcpy(whole,record,wlen);
        if(rp_incomplete_len) memcpy(whole+wlen,rp_incomplete,rp_incomplete_len);
        whole[wlen+rp_incomplete_len]='\0';
    }
    bool wrote=whole!=NULL && rp_write(path,whole,wlen+rp_incomplete_len);
    free(path);
    if(!wrote) {
        printf("%s  no record was written\n",name); t->broken++;
    } else {
        RpRecord again;
        bool read=rp_record_read(whole,&again);
        bool incomplete=rp_incomplete_len!=0;
        bool clean=read && !incomplete && !strcmp(again.verdict,"verdict: clean");
        t->clean+=clean; t->incomplete+=incomplete;
        printf("%s  %s",name,!read ? "unreadable" : incomplete ? "incomplete" : clean ? "clean" :
               again.verdict+strlen("verdict: "));
        if(!ran.arrived) { printf(" (it did not arrive)"); t->unarrived++; }
        if(ran.end==RP_BUSY) { printf(" (it asked for the next turn for ever)"); t->busy++; }
        if(ran.live_after!=live_before) { printf(" (%ld blocks not let go)",(long)(ran.live_after-live_before)); t->leaked++; }
        if(ran.harness_failures!=0) { printf(" (%u harness checks failed)",ran.harness_failures); t->broken++; }
        printf("\n");
        if(read) rp_record_free(&again);
    }
    free(whole);
    fake_reset();
    rp_index_free(&index); free(bytes); rp_record_free(&r);
}

static int rp_replay_dir(const char *kept, const char *out, uint64_t budget_ms, const char *only, RpTally *t) {
    DIR *d=opendir(kept);
    if(d==NULL) { fprintf(stderr,"replay: cannot read %s\n",kept); return 2; }
    if(!rp_exists(out) && mkdir(out,0777)!=0) { closedir(d); fprintf(stderr,"replay: cannot make %s\n",out); return 2; }
    char **names=NULL; size_t n=0;
    for(struct dirent *e; (e=readdir(d))!=NULL; ) {
        size_t len=strlen(e->d_name);
        if(len>4 && !strcmp(e->d_name+len-4,".txt") && (only==NULL || strstr(e->d_name,only)!=NULL)) {
            names=realloc(names,(n+1)*sizeof(*names)); names[n++]=strdup(e->d_name);
        }
    }
    closedir(d);
    qsort(names,n,sizeof(*names),rp_strcmp);
    for(size_t i=0;i<n;i++) { rp_replay_one(kept,out,names[i],budget_ms,t); fflush(stdout); free(names[i]); }
    free(names);
    printf("replayed %u, clean %u, incomplete %u",t->replayed,t->clean,t->incomplete);
    if(t->bare) printf("; %u records carry no kept bytes",t->bare);
    if(t->not_whole) printf("; %u not replayed, not whole or not readable",t->not_whole);
    if(t->unarrived) printf("; %u did not arrive",t->unarrived);
    if(t->busy) printf("; %u asked for the next turn for ever",t->busy);
    if(t->leaked) printf("; %u did not let go of what they held",t->leaked);
    if(t->broken) printf("; %u broke the harness",t->broken);
    printf("\n");
    return t->leaked || t->broken ? 1 : 0;
}

/* ── The self-test ──────────────────────────────────────────────────── */

/* A page or sub-resource the self-test serves: by address, its bytes, the
 * address it came from after redirects, and its response's label. */
typedef struct { const char *url; const char *bytes; const char *final; const char *label; } RpServed;
/* Files of a fixture directory, served at http://fixture.test/<file>, and a
 * table of hand-made ones besides. Anything else is the site's 404: a
 * fetch that fails, which the page then keeps as one. */
typedef struct { const char *dir; const RpServed *table; } RpSite;
static RpKnown rp_site_lookup(void *ctx, uint32_t kind, const char *url, RpAnswer *out) {
    (void)kind;
    RpSite *s=ctx;
    for(const RpServed *e=s->table; e!=NULL && e->url!=NULL; e++)
        if(!strcmp(e->url,url)) {
            *out=(RpAnswer){(const uint8_t *)e->bytes,strlen(e->bytes),e->final ? e->final : url,e->label ? e->label : ""};
            return RP_CAME;
        }
    static char *file; static size_t len;
    const char *prefix="http://fixture.test/";
    if(s->dir==NULL || strncmp(url,prefix,strlen(prefix))!=0 || strchr(url+strlen(prefix),'/')!=NULL ||
       strchr(url,'?')!=NULL) return RP_FAILED;
    char *path=rp_cat(s->dir,"/",url+strlen(prefix));
    free(file); file=rp_slurp(path,&len);
    free(path);
    if(file==NULL) return RP_FAILED;
    *out=(RpAnswer){(const uint8_t *)file,len,url,""};
    return RP_CAME;
}

/* The fake disk's /tmp/kept, written out under `host`: the records, the
 * kept bytes, the folders. */
static bool rp_copy_kept(const char *host) {
    const char *prefix="/tmp/kept/";
    bool ok=true;
    for(int pass=0;pass<2;pass++)
        for(int f=0;f<FAKE_FILES;f++) {
            if(!fake_files[f].used || strncmp(fake_files[f].path,prefix,strlen(prefix))) continue;
            const char *rest=fake_files[f].path+strlen(prefix);
            char *path=rp_cat(host,"/",rest);
            char *slash=strchr(path+strlen(host)+1,'/');
            if(pass==0 && slash!=NULL) { *slash='\0'; if(!rp_exists(path)) ok=mkdir(path,0777)==0 && ok; }
            if(pass==1) ok=rp_write(path,fake_files[f].data,fake_files[f].len) && ok;
            free(path);
        }
    return ok;
}

/* A hand-made page: what it proves, where it is from, its bytes and head,
 * the words of its own checks, which its record must not hold, words its
 * record must hold, which say it was read as it is meant to be, the width
 * of the view it is read in (0 for the harness's own), and whether it is
 * read with scripts off. */
typedef struct {
    const char *what;
    const char *address;
    const char *page;
    way_head_t head;
    const char *forbid, *require;   /* space-separated; NULL for none */
    int32_t width;
    bool scripts_off;
} RpCase;

static way_head_t rp_head(int status, const char *reason, const char *type, const char *charset,
                          const char *final, bool encrypted, way_body_t body) {
    way_head_t h; memset(&h,0,sizeof(h));
    h.status=status; os64_strcopy(h.reason,sizeof(h.reason),reason);
    os64_strcopy(h.content_type,sizeof(h.content_type),type);
    os64_strcopy(h.charset,sizeof(h.charset),charset);
    os64_strcopy(h.url,sizeof(h.url),final);
    h.encrypted=encrypted; h.body=body;
    if(encrypted) h.tls_version=0x0304;
    return h;
}

/* A kept page's record must be the replayed one, line for line, each
 * difference said. A `paint stages` line is the one exception, by its key:
 * its times are the processor's counter (YONDER_DIAGNOSTICS.md § What
 * paints cost), which no clock of the harness moves. */
static bool rp_same(const char *name, const char *a, const char *b) {
    bool same=true;
    const char *x=a, *y=b;
    while(*x || *y) {
        const char *xe=strchr(x,'\n'), *ye=strchr(y,'\n');
        size_t xl=xe ? (size_t)(xe-x) : strlen(x), yl=ye ? (size_t)(ye-y) : strlen(y);
        bool stages=xl>14 && yl>14 && !strncmp(x,"paint stages: ",14) && !strncmp(y,"paint stages: ",14);
        if(!stages && (xl!=yl || memcmp(x,y,xl))) {
            if(same) printf("  %s: the replay's record differs from the page's own\n",name);
            printf("    kept:   %.*s\n    replay: %.*s\n",(int)xl,x,(int)yl,y);
            same=false;
        }
        x+=xl+(xe?1:0); y+=yl+(ye?1:0);
    }
    return same;
}

/* A request the kept page's index has no line for: the first hand-made
 * page copied to WORK-DIR/partial with its script's line taken out of the
 * index, and replayed. The script is not made up, neither as content nor
 * as a failure: the record gains `REPLAY incomplete script <url>` and no
 * FAILED line for it, and the page is counted incomplete, not clean. The
 * problems found. */
static unsigned rp_incomplete_case(const char *work, const char *kept, const char *name) {
    unsigned bad=0;
    char *partial=rp_cat(work,"/partial",""), *out=rp_cat(work,"/partial-out","");
    char *stem=strndup(name,strlen(name)-4);
    char *from=rp_cat(kept,"/",stem), *to=rp_cat(partial,"/",stem);
    char *folder=rp_cat(to,".d","");
    mkdir(partial,0777); mkdir(folder,0777);
    static const char *const kFiles[]={".txt",".html",".d/1.js",".d/2.css",".d/3.css",".d/index"};
    for(size_t f=0;f<sizeof(kFiles)/sizeof(kFiles[0]);f++) {
        char *a=rp_cat(from,kFiles[f],""), *b=rp_cat(to,kFiles[f],"");
        size_t len=0; char *text=rp_slurp(a,&len);
        if(text==NULL) { printf("  incomplete case: %s%s was not kept\n",stem,kFiles[f]); bad++; }
        else {
            if(!strcmp(kFiles[f],".d/index")) {
                char *line=strstr(text,"\tscript\thttps://fixture.test/m.js\t");
                char *start=line; while(start!=NULL && start>text && start[-1]!='\n') start--;
                char *end=line!=NULL ? strchr(line,'\n') : NULL;
                if(end==NULL) { printf("  incomplete case: the index has no line for m.js\n"); bad++; }
                else { memmove(start,end+1,strlen(end+1)+1); len=strlen(text); }
            }
            if(!rp_write(b,text,len)) bad++;
        }
        free(text); free(a); free(b);
    }
    RpTally t={0};
    if(rp_replay_dir(partial,out,60000,NULL,&t)!=0) bad++;
    char *got=rp_cat(out,"/",name);
    char *record=rp_slurp(got,NULL);
    if(t.replayed!=1 || t.incomplete!=1 || t.clean!=0 || record==NULL ||
       strstr(record,"\nREPLAY incomplete script https://fixture.test/m.js\n")==NULL ||
       strstr(record,"FAILED script https://fixture.test/m.js")!=NULL) {
        printf("  incomplete case: a request the index lacks did not read as incomplete\n");
        bad++;
    }
    free(record); free(got);
    free(folder); free(from); free(to); free(stem); free(partial); free(out);
    return bad;
}

static int rp_self_test(const char *work) {
    char kept[4096], out[4096];
    snprintf(kept,sizeof(kept),"%s/kept",work); snprintf(out,sizeof(out),"%s/out",work);
    if((!rp_exists(kept) && mkdir(kept,0777)!=0) || (!rp_exists(out) && mkdir(out,0777)!=0)) {
        fprintf(stderr,"replay self-test: cannot make %s\n",kept); return 2;
    }
    /* The hand-made pages: a sheet that imports another, a label it is read
     * under, a page in windows-1252 from an encrypted fetch whose script
     * measures; a text body with a byte order mark; a file's page with a
     * script beside it; an error page; a page whose script's fetch failed,
     * with a module and a global it lacks; a page too deep to read to its
     * end; and a picture asked for by itself. */
    static const RpServed kServed[]={
        /* Its selector matches the page's windows-1252 class only when read
         * as the UTF-8 its response says it is. */
        {"https://fixture.test/k.css","@import 'i.css'; p.caf\xC3\xA9{width:300px;color:red}",
         "https://cdn.fixture.test/k.css","utf-8"},
        {"https://cdn.fixture.test/i.css","p{margin:0;aspect-ratio:1}",NULL,""},
        /* A check that fails names a global the page does not have, so a
         * replay that reads the page or its sheet otherwise is a record
         * with another MISSING line. */
        {"https://fixture.test/m.js","var o=document.getElementById('out');"
         "if(o.offsetWidth!=300)sheetUnread();if(o.textContent!='caf\\u00e9')charsetLost();",NULL,NULL},
        {"file:///pages/f.js","document.title='from the file'; setTimeout(function(){ document.body.className='later' },250)",
         NULL,NULL},
        {NULL,NULL,NULL,NULL}};
    static char deep[16384];
    size_t at=(size_t)snprintf(deep,sizeof(deep),"<p id=t>deep</p>");
    while(at+5<sizeof(deep)-1) { memcpy(deep+at,"<div>",5); at+=5; }
    deep[at]='\0';
    const RpCase hand[]={
        {"sheets, an @import, a label and a measuring script","http://fixture.test/kept",
         "<!doctype html><title>caf\xE9</title><link rel=stylesheet href=k.css>"
         "<p id=out class=caf\xE9>caf\xE9</p><script src=m.js></script><canvas></canvas>",
         rp_head(200,"OK","text/html","windows-1252","https://fixture.test/final",true,WAY_BODY_HTML),
         "sheetUnread charsetLost","aspect-ratio m.js:",0,false},
        {"a text body","http://fixture.test/notes.txt","\xEF\xBB\xBFplain <b>text</b> caf\xC3\xA9\n",
         rp_head(200,"OK","text/plain","","http://fixture.test/notes.txt",false,WAY_BODY_TEXT),NULL,"body:",0,false},
        {"a file's page, its script, and a timer that fires after it is shown","file:///pages/f.html",
         "<!doctype html><p>a file</p><script src=f.js></script><script>fetch</script>",
         rp_head(0,"","","","",false,WAY_BODY_HTML),NULL,"f.js: #timer-1:",0,false},
        {"an error page","http://fixture.test/busy","<!doctype html><p>slow down</p>",
         rp_head(429,"Too Many Requests","text/html","","http://fixture.test/busy",false,WAY_BODY_HTML),
         NULL,"429",0,false},
        {"a script that did not come, a module, a global","http://fixture.test/gone",
         "<!doctype html><script src=gone.js></script><script type=module>1</script>"
         "<script>if(window.localStorage)x()</script><svg></svg>",
         rp_head(200,"OK","text/html","","http://fixture.test/gone",false,WAY_BODY_HTML),NULL,"gone.js",0,false},
        {"a page too deep to read to its end","http://fixture.test/deep",deep,
         rp_head(200,"OK","text/html","","http://fixture.test/deep",false,WAY_BODY_HTML),NULL,"to its end",0,false},
        {"a picture asked for by itself","http://fixture.test/shot.png","",
         rp_head(200,"OK","image/png","","http://fixture.test/shot.png",false,WAY_BODY_IMAGE),NULL,"still coming",0,false},
        {"a wide window, which a media query and a script both see","http://fixture.test/wide",
         "<!doctype html><style>@media (max-width:1000px){p{aspect-ratio:2}}</style><p id=p>wide</p>"
         "<script>if(document.getElementById('p').offsetWidth<1000)narrow()</script>",
         rp_head(200,"OK","text/html","","http://fixture.test/wide",false,WAY_BODY_HTML),
         "narrow aspect-ratio","1400 px",1400,false},
        {"a page read with scripts off","http://fixture.test/quiet",
         "<!doctype html><p>no script runs</p><script>nothere()</script>",
         rp_head(200,"OK","text/html","","http://fixture.test/quiet",false,WAY_BODY_HTML),
         "nothere","scripts:",0,true},
    };
    const char *dirs[]={"tools/yonder_loop_fixture","tools/yonder_script_fixture","tools/pages"};
    unsigned pages=0, bad=0;
    uint32_t seq=0;
    char name[256], first_hand[256]="";
    /* Each page loaded as a person browsing with keep_pages on would load
     * it, and what yonder kept written out. */
    for(int set=0;set<4;set++) {
        DIR *d=set<3 ? opendir(dirs[set]) : NULL;
        if(set<3 && d==NULL) { printf("  cannot read %s\n",dirs[set]); bad++; continue; }
        char **files=NULL; size_t nfiles=0;
        for(struct dirent *e; d!=NULL && (e=readdir(d))!=NULL; ) {
            size_t len=strlen(e->d_name);
            if(len>5 && !strcmp(e->d_name+len-5,".html")) { files=realloc(files,(nfiles+1)*sizeof(*files)); files[nfiles++]=strdup(e->d_name); }
        }
        if(d!=NULL) closedir(d);
        if(files!=NULL) qsort(files,nfiles,sizeof(*files),rp_strcmp);
        size_t count=set<3 ? nfiles : sizeof(hand)/sizeof(hand[0]);
        for(size_t i=0;i<count;i++) {
            char address[512], *file=NULL; size_t len=0;
            RpLoad load={0};
            RpSite site={set<3 ? dirs[set] : NULL,kServed};
            if(set<3) {
                char *path=rp_cat(dirs[set],"/",files[i]);
                file=rp_slurp(path,&len);
                free(path);
                snprintf(address,sizeof(address),"http://fixture.test/%s",files[i]);
                load.head=rp_head(200,"OK","text/html","",address,false,WAY_BODY_HTML);
                load.bytes=(const uint8_t *)file; load.len=len;
            } else {
                snprintf(address,sizeof(address),"%s",hand[i].address);
                load.head=hand[i].head;
                load.bytes=(const uint8_t *)hand[i].page;
                load.len=strlen(hand[i].page);
                load.width=hand[i].width;
            }
            load.address=address; load.seq=++seq; load.local=!strncmp(address,"file://",7);
            load.scripts=set<3 || !hand[i].scripts_off;
            load.dir="/tmp/kept"; load.keep=true; load.budget_ms=60000;
            fake_reset(); snprintf(fake_dirs[nfake_dirs++],256,"/tmp/kept");
            size_t live_before=live;
            RpRan ran=rp_run(&load,rp_site_lookup,&site);
            pages++;
            size_t wlen;
            const char *record=rp_written("/tmp/kept",address,load.seq,name,sizeof(name),&wlen);
            if(set==3 && i==0 && record!=NULL) snprintf(first_hand,sizeof(first_hand),"%s",name);
            if(record==NULL || !rp_copy_kept(kept) || ran.harness_failures!=0 || ran.live_after!=live_before) {
                printf("  %s (%s): %s\n",address,set<3 ? files[i] : hand[i].what,
                    record==NULL ? "no record" : ran.harness_failures ? "harness checks failed" :
                    ran.live_after!=live_before ? "did not let go of what it held" : "not written out");
                bad++;
            }
            /* The page read as it is meant to where it was kept: a replay
             * that matches it is matching a page that read right. */
            for(int must=0;must<2 && set==3;must++) {
                const char *list=must ? hand[i].require : hand[i].forbid;
                char *words=list!=NULL ? strdup(list) : NULL;
                for(char *w=words!=NULL ? strtok(words," ") : NULL; w!=NULL; w=strtok(NULL," "))
                    if(record==NULL || (strstr(record,w)!=NULL)!=(must==1)) {
                        printf("  %s (%s): where it was kept, its record %s %s\n",address,hand[i].what,
                            must ? "lacks" : "holds",w);
                        bad++;
                    }
                free(words);
            }
            fake_reset();
            free(file);
            if(set<3) free(files[i]);
        }
        free(files);
    }
    /* Every one replayed from what was written out, the way a corpus is. */
    RpTally t={0};
    int rc=rp_replay_dir(kept,out,60000,NULL,&t);
    if(rc!=0 || t.replayed!=pages || t.incomplete!=0) bad++;
    DIR *d=opendir(kept);
    for(struct dirent *e; d!=NULL && (e=readdir(d))!=NULL; ) {
        size_t len=strlen(e->d_name);
        if(len<5 || strcmp(e->d_name+len-4,".txt")) continue;
        size_t alen=0, blen=0;
        char *a=rp_cat(kept,"/",e->d_name), *b=rp_cat(out,"/",e->d_name);
        char *x=rp_slurp(a,&alen), *y=rp_slurp(b,&blen);
        if(x==NULL || y==NULL || !rp_same(e->d_name,x,y)) bad++;
        free(x); free(y); free(a); free(b);
    }
    if(d!=NULL) closedir(d);
    bad+=rp_incomplete_case(work,kept,first_hand);
    printf("replay self-test: %u pages kept and replayed, %u problems\n",pages,bad);
    return bad!=0;
}

int main(int argc, char **argv) {
    int rc;
    if(argc==3 && !strcmp(argv[1],"--self-test")) {
        rc=rp_self_test(argv[2]);
    } else if(argc>=3) {
        uint64_t seconds=60; const char *only=NULL;
        for(int i=3;i<argc;i++) {
            if(!strcmp(argv[i],"--seconds") && i+1<argc) seconds=strtoull(argv[++i],NULL,10);
            else if(!strcmp(argv[i],"--only") && i+1<argc) only=argv[++i];
            else { fprintf(stderr,"replay: what is %s?\n",argv[i]); return 2; }
        }
        RpTally t={0};
        rc=rp_replay_dir(argv[1],argv[2],seconds*1000,only,&t);
    } else {
        fprintf(stderr,"usage: %s KEPT-DIR OUT-DIR [--seconds N] [--only TEXT]\n"
                       "       %s --self-test WORK-DIR\n",argv[0],argv[0]);
        return 2;
    }
    if(live!=0) fprintf(stderr,"replay: %zu blocks still held at the end\n",live);
    return rc!=0 ? rc : failures!=0 || live!=0;
}
