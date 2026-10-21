/* THE WPT RUNNER (MODERN_WEB.md § The oracle): web-platform-tests pages
 * loaded into yonder's own window, headless, and their results read back.
 *
 *   test_yonder_wpt_host --wpt <checkout> --dir <dir> --expect <file>
 *                        [--pages <file>] [--update] [--only <substring>]
 *                        [--report <file>] [--modern-off] [--fault]
 *                        [--short-read <path>]
 *
 * The window is the scripted-page host's (that file is included whole,
 * without its main, for its fakes of the OS: the pool, the mailbox, the
 * clocks, the disk), so a page here takes the path a page from the network
 * takes in yonder: a navigation, a head, the body in the stream's chunks,
 * the verdict, then turns of the loop. Its modern switch is on, as yonder's
 * is unless yonder.conf says otherwise; --modern-off turns it off, which is
 * how a slice shows its required tests are its own surface's: then every
 * [required] test that passes is listed as LEAKED and fails the run, the
 * baseline is not compared (it is the switch on's), and --update is
 * refused.
 *
 * WHAT IS RUN: with --pages, the pages that file lists, which
 * tools/wpt/pages.py makes from WPT's own MANIFEST.json (the suite's list of
 * its tests, their types and the pages each script test makes). Without
 * it, the same found by file name: under <dir>, every .html or .htm that
 * names /resources/testharness.js, and every .any.js and .window.js through
 * the page WPT generates for its window variant; nothing under a support,
 * resources, reference, crashtests or tools directory, no -ref, -notref or
 * -manual file, and no .worker.js (there are no workers). A .any.js whose
 * META global= names no window is not run either. Each variant (a META
 * variant= line, a <meta name=variant>) is a page of its own. A page is
 * named as WPT names it, relative to <dir>: `a.html`, `b.any.html?1-10`.
 * A test file that cannot be read whole is listed all the same, as the page
 * it would most simply make, so its run is UNJUDGED below, as a
 * manifest-listed page's is. A page the expectation file excludes is not
 * run.
 *
 * WHAT IS SERVED: the page is http://wpt.test/<dir>/<name>; every script and
 * sheet the page asks for whose address is under http://wpt.test/ is
 * answered from the checkout (a .py handler is not: nothing here runs
 * one), /resources/testharnessreport.js from --report (tools/wpt/'s, which
 * leaves the results in the tree), and everything else fails as a fetch
 * that did not arrive does. Pictures and fonts always fail. The bytes are
 * served as they are on disk, which WPT writes in UTF-8.
 *
 * THE CLOCK: the loop is turned until nothing is owed, then the window's
 * clock moves one 10 ms tick and it is turned again, up to a budget of 60 s
 * of that clock (180 s for a page that says its timeout is long), so the
 * harness's own timeout and every step_timeout fire as they would, in the
 * same order every run. The microsecond clock a script task's deadline is
 * measured against moves 1 ms at each read, so a script that never ends is
 * ended, and its page has no results.
 *
 * THE RESULTS: one line per test, `STATUS<tab>page<tab>name`, and a tab
 * and the harness's message when it gave one; a page that never reported
 * is one TIMEOUT line naming why; a harness that ended in anything but OK
 * is a HARNESS line with its message, and that page FAILS WHOLE: none of
 * its tests is a pass, and one the harness said passed is a FAIL line that
 * says so. A page never run to a verdict (its source missing, past
 * WPT_FILE_MAX, or not read whole; its navigation never started) is an
 * UNJUDGED line, and the run cannot be judged (exit 2). Then the summary.
 *
 * THE EXPECTATIONS (--expect) are three sections of `page<tab>...` lines,
 * `#` lines being comments (MODERN_WEB.md § The oracle):
 *   [required]  page<tab>test: what the slice's interface must do; each
 *               one that did not pass is listed as REQUIRED and the run
 *               fails.
 *   [passing]   page<tab>test: the baseline; one that did not pass is
 *               listed as LOST and the run fails, and a pass it lacks is
 *               listed as NEW.
 *   [excluded]  page<tab>reason: a page the slice knowingly does not run,
 *               and why.
 * A file that does not exist is three empty sections; one that exists and
 * cannot be read makes the run one that cannot be judged (exit 2).
 * --update rewrites [passing] from this run and copies the other two as
 * they are, since they are written by hand; with --only it keeps the
 * [passing] lines of pages the substring does not select. The only thing
 * that may change the file is a page run to a verdict, so it writes last,
 * beside the file and renamed over it, and only for a run every page of
 * which was: an UNJUDGED page, or a fault in the window's fakes (the host's
 * check() failing, a page's or the teardown's), fails the run (exit 2)
 * whatever the results and leaves the file as it was. --fault fakes a
 * fault, for the selftest. */
#define YONDER_HOST_FAKES_ONLY
#include "test_yonder_scripts_host.c"

#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <stdarg.h>
#include <strings.h>
#include <sys/stat.h>

#define WPT_ORIGIN "http://wpt.test/"
#define WPT_REPORT_PATH "resources/testharnessreport.js"
#define WPT_RESULTS_ID "os64-wpt-results"
#define WPT_TICK_MS 10
#define WPT_BUDGET_MS 60000
#define WPT_LONG_BUDGET_MS 180000
#define WPT_FILE_MAX (16u * 1024u * 1024u)

typedef enum { WPT_HTML, WPT_ANY, WPT_WINDOW } wpt_kind_t;

typedef struct {
    char *name;                 /* as WPT names the page, relative to <dir> */
    char *source;               /* its file */
    char *rel;                  /* its file, relative to the checkout */
    wpt_kind_t kind;
    int long_timeout;           /* 1 or 0 as the manifest says; -1, read the page */
} wpt_page_t;

typedef struct { char **v; size_t n, cap; } wpt_list_t;

static const char *wpt_root, *wpt_dir, *wpt_report = "tools/wpt/testharnessreport.js";
static wpt_page_t *wpt_pages;
static size_t wpt_npages, wpt_pages_cap;
/* The page under way asked to navigate, which ends its turns. */
static bool wpt_navigated;
/* The window's modern switch is off (--modern-off). */
static bool wpt_modern_off;
/* --fault: a fault in the fakes after the last page, as if one had been
 * met, for the selftest to prove a faulted run rewrites nothing. */
static bool wpt_fault;
/* --short-read <path>: every read of exactly that path stops a byte short,
 * as a file changing under the reader would, for the selftest to prove a
 * short read is a failed one. */
static const char *wpt_short_read;

static void *wpt_must(void *p) {
    if(p==NULL) { fprintf(stderr,"wpt: out of memory\n"); exit(2); }
    return p;
}
static char *wpt_strdup(const char *s) { return wpt_must(strdup(s)); }
static char *wpt_strndup(const char *s, size_t n) { return wpt_must(strndup(s,n)); }
/* A formatted string, malloc'd whatever its length. */
static char *wpt_fmt(const char *fmt, ...) __attribute__((format(printf,1,2)));
static char *wpt_fmt(const char *fmt, ...) {
    va_list a, b;
    va_start(a,fmt); va_copy(b,a);
    int n=vsnprintf(NULL,0,fmt,a);
    va_end(a);
    if(n<0) { fprintf(stderr,"wpt: cannot format %s\n",fmt); exit(2); }
    char *s=wpt_must(malloc((size_t)n+1));
    vsnprintf(s,(size_t)n+1,fmt,b);
    va_end(b);
    return s;
}
static void wpt_list_add(wpt_list_t *l, char *s) {
    if(l->n==l->cap) { l->cap=l->cap?l->cap*2:64; l->v=wpt_must(realloc(l->v,l->cap*sizeof(*l->v))); }
    l->v[l->n++]=s;
}
static int wpt_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a,*(char *const *)b); }
/* Sorted, each string once. */
static void wpt_list_settle(wpt_list_t *l) {
    if(l->n==0) return;
    qsort(l->v,l->n,sizeof(*l->v),wpt_cmp);
    size_t k=1;
    for(size_t i=1;i<l->n;i++) {
        if(strcmp(l->v[i],l->v[k-1])==0) free(l->v[i]);
        else l->v[k++]=l->v[i];
    }
    l->n=k;
}
static void wpt_list_free(wpt_list_t *l) {
    for(size_t i=0;i<l->n;i++) free(l->v[i]);
    free(l->v); memset(l,0,sizeof(*l));
}

/* A whole file, NUL-terminated; NULL when it is not a regular file, is
 * past WPT_FILE_MAX, cannot be opened, or does not read whole (fewer bytes
 * than its size, or a read error). */
static char *wpt_slurp(const char *path, size_t *len) {
    struct stat st;
    if(stat(path,&st)!=0 || !S_ISREG(st.st_mode) || (uint64_t)st.st_size>WPT_FILE_MAX) return NULL;
    FILE *f=fopen(path,"rb");
    if(f==NULL) return NULL;
    size_t want=(size_t)st.st_size;
    char *bytes=wpt_must(malloc(want+1));
    size_t asked=want>0 && wpt_short_read!=NULL && !strcmp(path,wpt_short_read) ? want-1 : want;
    size_t got=fread(bytes,1,asked,f);
    bool whole=got==want && !ferror(f);
    fclose(f);
    /* A read that stopped short is no file at all: what it lost may be the
     * part that mattered. */
    if(!whole) { free(bytes); return NULL; }
    bytes[got]='\0';
    *len=got;
    return bytes;
}
static bool wpt_ends(const char *s, const char *tail) {
    size_t n=strlen(s), t=strlen(tail);
    return n>=t && strcmp(s+n-t,tail)==0;
}
/* `a/b`, or `b` when `a` is empty; malloc'd. */
static char *wpt_join(const char *a, const char *b) { return a[0] ? wpt_fmt("%s/%s",a,b) : wpt_strdup(b); }

// ── Finding the pages ─────────────────────────────────────────────────────

/* Takes `name`; copies the rest. */
static void wpt_page_add(char *name, const char *source, const char *rel, wpt_kind_t kind, int long_timeout) {
    if(wpt_npages==wpt_pages_cap) {
        wpt_pages_cap=wpt_pages_cap?wpt_pages_cap*2:256;
        wpt_pages=wpt_must(realloc(wpt_pages,wpt_pages_cap*sizeof(*wpt_pages)));
    }
    wpt_pages[wpt_npages++]=(wpt_page_t){name,wpt_strdup(source),wpt_strdup(rel),kind,long_timeout};
}

static void wpt_pages_free(void) {
    for(size_t i=0;i<wpt_npages;i++) { free(wpt_pages[i].name); free(wpt_pages[i].source); free(wpt_pages[i].rel); }
    free(wpt_pages);
    wpt_pages=NULL; wpt_npages=wpt_pages_cap=0;
}

/* What tools/wpt/pages.py made of the manifest: its `run` lines as pages,
 * and its `skip` lines counted by their reason, into `skipped` as
 * `reason` strings, once per page. False when it cannot be read or a line
 * is not one of the two. */
static bool wpt_read_pages(const char *path, wpt_list_t *skipped) {
    size_t len=0;
    char *text=wpt_slurp(path,&len);
    if(text==NULL) { fprintf(stderr,"wpt: cannot read %s\n",path); return false; }
    bool ok=true;
    for(char *save=NULL,*line=strtok_r(text,"\n",&save);line && ok;line=strtok_r(NULL,"\n",&save)) {
        char *field[5]={0};
        int n=0;
        for(char *p=line;n<5;) {
            field[n++]=p;
            char *tab=strchr(p,'\t');
            if(tab==NULL) break;
            *tab='\0'; p=tab+1;
        }
        if(n==3 && !strcmp(field[0],"skip")) {
            wpt_list_add(skipped,wpt_strdup(field[2]));
        } else if(n==5 && !strcmp(field[0],"run")) {
            wpt_kind_t kind=!strcmp(field[3],"any") ? WPT_ANY : !strcmp(field[3],"window") ? WPT_WINDOW : WPT_HTML;
            char *source=wpt_join(wpt_root,field[2]);
            wpt_page_add(wpt_strdup(field[1]),source,field[2],kind,!strcmp(field[4],"long"));
            free(source);
        } else {
            fprintf(stderr,"wpt: %s: a line that is neither run nor skip\n",path);
            ok=false;
        }
    }
    free(text);
    return ok;
}

/* The value of the attribute `name` in the tag text [tag, end), malloc'd;
 * NULL when the tag has none. Enough of HTML's attribute syntax for the
 * suite's own <meta> lines: quoted either way, or bare. */
static char *wpt_attr(const char *tag, const char *end, const char *name) {
    size_t n=strlen(name);
    for(const char *p=tag;p+n<end;p++) {
        if(strncasecmp(p,name,n)!=0 || !(p[-1]==' ' || p[-1]=='\t' || p[-1]=='\n' || p[-1]=='\r')) continue;
        const char *q=p+n;
        while(q<end && (*q==' ' || *q=='\t')) q++;
        if(q>=end || *q!='=') continue;
        q++;
        while(q<end && (*q==' ' || *q=='\t')) q++;
        char quote=(q<end && (*q=='"' || *q=='\'')) ? *q++ : 0;
        const char *v=q;
        while(q<end && (quote ? *q!=quote : !(*q==' ' || *q=='\t' || *q=='\n' || *q=='>' || *q=='/'))) q++;
        return wpt_strndup(v,(size_t)(q-v));
    }
    return NULL;
}

/* An HTML test's <meta name=`name` content=...> lines: whether one says
 * `content` (NULL: any), each one's content added to `all` when it is
 * given. A variant is one, and so is a long timeout. */
static bool wpt_html_meta(const char *html, const char *name, const char *content, wpt_list_t *all) {
    bool found=false;
    for(const char *p=html;(p=strstr(p,"<meta"))!=NULL;p++) {
        const char *end=strchr(p,'>');
        if(end==NULL) break;
        char *said=wpt_attr(p+5,end,"name"), *value=wpt_attr(p+5,end,"content");
        if(said!=NULL && value!=NULL && strcasecmp(said,name)==0 &&
           (content==NULL || strcmp(value,content)==0)) {
            found=true;
            if(all!=NULL) { wpt_list_add(all,value); value=NULL; }
        }
        free(said); free(value);
    }
    return found;
}

/* A script test's META lines (WPT's read_script_metadata): the leading
 * run of `// META: key=value` lines, ending at the first line that is not
 * one. */
typedef struct {
    wpt_list_t scripts, variants;
    char *title;
    bool long_timeout, any_global, window;
} wpt_meta_t;
static void wpt_script_meta(const char *src, wpt_meta_t *m) {
    memset(m,0,sizeof(*m));
    for(const char *line=src;*line!='\0';) {
        const char *eol=strchr(line,'\n');
        if(eol==NULL) eol=line+strlen(line);
        const char *p=line;
        if(strncmp(p,"//",2)!=0) break;
        p+=2;
        while(p<eol && (*p==' ' || *p=='\t')) p++;
        if(eol-p<5 || strncmp(p,"META:",5)!=0) break;
        p+=5;
        while(p<eol && (*p==' ' || *p=='\t')) p++;
        const char *eq=memchr(p,'=',(size_t)(eol-p));
        if(eq==NULL) break;
        const char *key_end=eq;
        while(key_end>p && (key_end[-1]==' ' || key_end[-1]=='\t')) key_end--;
        const char *v=eq+1, *v_end=eol;
        while(v<v_end && (*v==' ' || *v=='\t')) v++;
        while(v_end>v && (v_end[-1]==' ' || v_end[-1]=='\t' || v_end[-1]=='\r')) v_end--;
        char *key=wpt_strndup(p,(size_t)(key_end-p)), *value=wpt_strndup(v,(size_t)(v_end-v));
        if(!strcmp(key,"script")) { wpt_list_add(&m->scripts,value); value=NULL; }
        else if(!strcmp(key,"variant")) { wpt_list_add(&m->variants,value); value=NULL; }
        else if(!strcmp(key,"title")) { free(m->title); m->title=value; value=NULL; }
        else if(!strcmp(key,"timeout")) m->long_timeout=!strcmp(value,"long");
        else if(!strcmp(key,"global")) {
            m->any_global=true;
            for(char *save=NULL,*t=strtok_r(value,",",&save);t;t=strtok_r(NULL,",",&save)) {
                while(*t==' ') t++;
                size_t tl=strlen(t);
                while(tl>0 && t[tl-1]==' ') t[--tl]='\0';
                if(!strcmp(t,"window") || !strcmp(t,"default")) m->window=true;
            }
        }
        free(key); free(value);
        line=*eol ? eol+1 : eol;
    }
}
static void wpt_meta_free(wpt_meta_t *m) {
    wpt_list_free(&m->scripts); wpt_list_free(&m->variants); free(m->title);
}

static bool wpt_skipped_dir(const char *name) {
    return name[0]=='.' || !strcmp(name,"support") || !strcmp(name,"resources") ||
        !strcmp(name,"reference") || !strcmp(name,"crashtests") || !strcmp(name,"tools");
}
static bool wpt_skipped_file(const char *name) {
    return strstr(name,"-ref.")!=NULL || strstr(name,"-notref.")!=NULL || strstr(name,"-manual.")!=NULL;
}

/* One file found, `rel` below <dir>: the pages it makes, if it is a test
 * this runner runs. */
static void wpt_consider(const char *source, const char *rel) {
    const char *slash=strrchr(rel,'/'), *name=slash ? slash+1 : rel;
    if(wpt_skipped_file(name)) return;
    bool any=wpt_ends(name,".any.js"), window=wpt_ends(name,".window.js");
    bool html=wpt_ends(name,".html") || wpt_ends(name,".htm");
    if(!any && !window && !html) return;
    size_t len=0;
    char *bytes=wpt_slurp(source,&len);
    char *checkout_rel=wpt_join(wpt_dir,rel);
    if(bytes==NULL) {
        /* A test file that cannot be read is listed all the same, under the
         * page it would most simply make, and its run says UNJUDGED, as a
         * manifest-listed page's does: left out, it would read as a page
         * gone and --update would drop its passes. */
        int stem=(int)(strlen(rel)-strlen(any ? ".any.js" : ".window.js"));
        wpt_page_add(any || window ? wpt_fmt("%.*s%s",stem,rel,any ? ".any.html" : ".window.html")
                                   : wpt_strdup(rel),
                     source,checkout_rel,any ? WPT_ANY : window ? WPT_WINDOW : WPT_HTML,-1);
    } else if(any || window) {
        wpt_meta_t m;
        wpt_script_meta(bytes,&m);
        if(!any || !m.any_global || m.window) {
            int stem=(int)(strlen(rel)-strlen(any ? ".any.js" : ".window.js"));
            if(m.variants.n==0) wpt_list_add(&m.variants,wpt_strdup(""));
            for(size_t i=0;i<m.variants.n;i++)
                wpt_page_add(wpt_fmt("%.*s%s%s",stem,rel,any ? ".any.html" : ".window.html",m.variants.v[i]),
                    source,checkout_rel,any ? WPT_ANY : WPT_WINDOW,-1);
        }
        wpt_meta_free(&m);
    } else if(strstr(bytes,"/resources/testharness.js")!=NULL) {
        wpt_list_t variants={0};
        wpt_html_meta(bytes,"variant",NULL,&variants);
        if(variants.n==0) wpt_list_add(&variants,wpt_strdup(""));
        for(size_t i=0;i<variants.n;i++)
            wpt_page_add(wpt_fmt("%s%s",rel,variants.v[i]),source,checkout_rel,WPT_HTML,-1);
        wpt_list_free(&variants);
    }
    free(checkout_rel);
    free(bytes);
}

/* Every file under `path`, which is `rel` below <dir>, in name order. */
static void wpt_walk(const char *path, const char *rel) {
    DIR *d=opendir(path);
    if(d==NULL) return;
    wpt_list_t names={0};
    for(struct dirent *e;(e=readdir(d))!=NULL;)
        if(strcmp(e->d_name,".") && strcmp(e->d_name,"..")) wpt_list_add(&names,wpt_strdup(e->d_name));
    closedir(d);
    wpt_list_settle(&names);
    for(size_t i=0;i<names.n;i++) {
        char *child=wpt_join(path,names.v[i]), *child_rel=wpt_join(rel,names.v[i]);
        struct stat st;
        if(stat(child,&st)==0 && S_ISDIR(st.st_mode) && !wpt_skipped_dir(names.v[i]))
            wpt_walk(child,child_rel);
        else if(stat(child,&st)==0 && S_ISREG(st.st_mode))
            wpt_consider(child,child_rel);
        free(child); free(child_rel);
    }
    wpt_list_free(&names);
}

// ── Serving the page and what it asks for ─────────────────────────────────

/* What an address serves, malloc'd with its length: a file of the
 * checkout, or the report script this runner serves in place of the
 * suite's; NULL for anything this runner does not serve. */
static char *wpt_serve(const char *url, size_t *len) {
    size_t o=strlen(WPT_ORIGIN);
    if(strncmp(url,WPT_ORIGIN,o)!=0) return NULL;
    size_t cap=strlen(url)+1, n=0;
    char *path=wpt_must(malloc(cap));
    for(const char *p=url+o;*p!='\0' && *p!='?' && *p!='#';p++) {
        int c=(unsigned char)*p;
        if(c=='%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3]={p[1],p[2],0};
            c=(int)strtol(hex,NULL,16); p+=2;
        }
        if(c==0) { free(path); return NULL; }
        path[n++]=(char)c;
    }
    path[n]='\0';
    char *bytes=NULL;
    /* Nothing above the checkout, and no server-side handler. */
    if(!strcmp(path,"..") || !strncmp(path,"../",3) || strstr(path,"/../")!=NULL ||
       wpt_ends(path,"/..") || wpt_ends(path,".py")) {
    } else if(!strcmp(path,WPT_REPORT_PATH)) {
        bytes=wpt_slurp(wpt_report,len);
    } else {
        char *file=wpt_join(wpt_root,path);
        bytes=wpt_slurp(file,len);
        free(file);
    }
    free(path);
    return bytes;
}

static void *wpt_product(size_t size) {
    void *p=os64_calloc(1,size);
    if(p==NULL) { fprintf(stderr,"wpt: out of memory\n"); exit(2); }
    return p;
}

/* The worker's answer to every fetch the window has out, as the pool would
 * reap them: true when there was one. */
static bool wpt_answer(void) {
    bool any=false;
    while(nscript_jobs>0) {
        os64_work_id_t id=script_jobs[0].id; yonder_script_job_t *job=script_jobs[0].job;
        script_jobs[0]=script_jobs[--nscript_jobs];
        size_t len=0;
        char *bytes=wpt_serve(job->url,&len);
        yonder_script_t *got=wpt_product(sizeof(*got));
        if(bytes!=NULL) {
            got->ok=true; got->length=len; got->source=wpt_product(len+1);
            memcpy(got->source,bytes,len+1);
        }
        free(bytes);
        os64_strcopy(got->url,sizeof(got->url),job->url);
        reaped(id,job,got);
        any=true;
    }
    while(nsheet_jobs>0) {
        os64_work_id_t id=sheet_jobs[0].id; yonder_sheet_job_t *job=sheet_jobs[0].job;
        sheet_jobs[0]=sheet_jobs[--nsheet_jobs];
        size_t len=0;
        char *bytes=wpt_serve(job->url,&len);
        yonder_sheet_t *got=wpt_product(sizeof(*got));
        if(bytes!=NULL)
            got->ok=garb_parse_sheet((const uint8_t *)bytes,len,NULL,NULL,&got->parsed)==GARB_OK;
        free(bytes);
        os64_strcopy(got->url,sizeof(got->url),job->url);
        reaped(id,job,got);
        any=true;
    }
    while(nfont_jobs>0) {
        os64_work_id_t id=font_jobs[0].id; yonder_webfont_job_t *job=font_jobs[0].job;
        font_jobs[0]=font_jobs[--nfont_jobs];
        yonder_webfont_t *got=wpt_product(sizeof(*got));
        os64_strcopy(got->why,sizeof(got->why),"the WPT runner serves no fonts");
        reaped(id,job,got);
        any=true;
    }
    /* The pool's one held job: a picture (failed, as a fetch that did not
     * arrive), or a navigation the page asked for, which ends its turns. */
    if(pool_work.job!=NULL) {
        uint32_t kind=*(const uint32_t *)pool_work.job;
        if(kind==YONDER_JOB_PICTURE) {
            void *job=pool_work.job;
            memset(&pool_work,0,sizeof(pool_work));
            reaped(pool_work_id,job,NULL);
            any=true;
        } else if(kind==YONDER_JOB_TRIP) {
            wpt_navigated=true;
        }
    }
    return any;
}

/* Turns of the loop until none has anything more to do now: false when
 * they never stop. */
static bool wpt_turns(void) {
    for(int i=0;i<100000;i++) {
        bool more=stream_turn();
        more=coming_turn() || more;
        more=script_turn() || more;
        more=wpt_answer() || more;
        if(wpt_navigated || !more) return true;
    }
    return false;
}

/* `s` as an attribute's value or a title's text, malloc'd. */
static char *wpt_escape(const char *s) {
    char *out=wpt_must(malloc(strlen(s)*6+1)), *at=out;
    for(;*s;s++) {
        const char *e=*s=='&'?"&amp;":*s=='<'?"&lt;":*s=='"'?"&quot;":NULL;
        if(e) { memcpy(at,e,strlen(e)); at+=strlen(e); } else *at++=*s;
    }
    *at='\0';
    return out;
}
/* `text` added to the malloc'd `*html`, which takes it. */
static void wpt_append(char **html, char *text) {
    char *joined=wpt_fmt("%s%s",*html,text);
    free(*html); free(text);
    *html=joined;
}

/* The page WPT generates for a script test's window variant (its serve
 * tool's any.html and window.html), malloc'd. */
static char *wpt_wrapper(const wpt_page_t *page, bool *long_timeout) {
    size_t len=0;
    char *src=wpt_slurp(page->source,&len);
    if(src==NULL) return NULL;
    wpt_meta_t m;
    wpt_script_meta(src,&m);
    free(src);
    *long_timeout=m.long_timeout;
    char *html=wpt_strdup("<!doctype html>\n<meta charset=utf-8>\n"), *esc;
    if(m.title!=NULL) { esc=wpt_escape(m.title); wpt_append(&html,wpt_fmt("<title>%s</title>\n",esc)); free(esc); }
    if(m.long_timeout) wpt_append(&html,wpt_strdup("<meta name=\"timeout\" content=\"long\">\n"));
    if(page->kind==WPT_ANY)
        wpt_append(&html,wpt_strdup("<script>\nself.GLOBAL = {\n"
            "  isWindow: function() { return true; },\n"
            "  isWorker: function() { return false; },\n"
            "  isShadowRealm: function() { return false; },\n};\n</script>\n"));
    wpt_append(&html,wpt_strdup("<script src=\"/resources/testharness.js\"></script>\n"
        "<script src=\"/resources/testharnessreport.js\"></script>\n"));
    for(size_t i=0;i<m.scripts.n;i++) {
        esc=wpt_escape(m.scripts.v[i]);
        wpt_append(&html,wpt_fmt("<script src=\"%s\"></script>\n",esc));
        free(esc);
    }
    esc=wpt_escape(page->rel);
    wpt_append(&html,wpt_fmt("<div id=log></div>\n<script src=\"/%s\"></script>\n",esc));
    free(esc);
    wpt_meta_free(&m);
    return html;
}

// ── Running one page ──────────────────────────────────────────────────────

/* A page's outcome: its result lines when it reported; why it did not when
 * it ran and did not; and `unjudged` when it was never run to a verdict at
 * all (its source could not be read, the window would not start it), which
 * is the run's failure and not the page's. */
typedef struct { wpt_list_t lines; bool reported, unjudged; const char *why; } wpt_outcome_t;

/* The results element's text, if the page has one: its text children
 * joined, malloc'd. */
static char *wpt_results_text(void) {
    const os64_html_document_t *docs[2]={page_doc(&g.page),g.coming.active ? page_doc(&g.coming.page) : NULL};
    for(int d=0;d<2;d++) {
        os64_html_node_t *n=probe_id_in(docs[d],WPT_RESULTS_ID);
        if(n==NULL) continue;
        size_t total=0;
        for(os64_html_node_t *c=n->first_child;c;c=c->next) if(c->kind==OS64_HTML_TEXT) total+=c->text_len;
        char *text=wpt_must(malloc(total+1)), *at=text;
        for(os64_html_node_t *c=n->first_child;c;c=c->next)
            if(c->kind==OS64_HTML_TEXT) { memcpy(at,c->text,c->text_len); at+=c->text_len; }
        *at='\0';
        return text;
    }
    return NULL;
}

static void wpt_run(const wpt_page_t *page, wpt_outcome_t *out) {
    memset(out,0,sizeof(*out));
    bool long_timeout=false;
    size_t len=0;
    char *html=page->kind==WPT_HTML ? wpt_slurp(page->source,&len) : wpt_wrapper(page,&long_timeout);
    if(html==NULL) {
        out->unjudged=true;
        out->why="its source is missing, past the size a file may have here, or did not read whole";
        return;
    }
    len=strlen(html);
    if(page->kind==WPT_HTML) long_timeout=wpt_html_meta(html,"timeout","long",NULL);
    if(page->long_timeout>=0) long_timeout=page->long_timeout;
    char *url=wpt_fmt("%s%s/%s",WPT_ORIGIN,wpt_dir,page->name);
    stream_window();
    g.scripts_on=true; g.script_ms=5000; now_ms=100; clock_step=1000;
    g.modern=!wpt_modern_off;
    wpt_navigated=false;
    start_trip(url,NULL,NAV_GO,NULL);
    if(pool_work.job==NULL || !g.stream.active) {
        out->unjudged=true;
        out->why="the window did not start the navigation";
        free(html); free(url); stream_window_drop(); clock_step=0;
        return;
    }
    way_head_t h=stream_head("text/html","",false);
    os64_strcopy(h.url,sizeof(h.url),url);
    yonder_mail_post_head(g.stream.mail,&h);
    stream_post_body((const uint8_t *)html,len,YONDER_STREAM_CHUNK);
    stream_post_verdict(true,OS64_FETCH_OK,"");
    free(html); free(url);
    /* The fetch has said everything it will; the pool lets its job go, as
     * it does a worker's that returned. */
    void *trip=pool_work.job;
    memset(&pool_work,0,sizeof(pool_work));
    reaped(pool_work_id,trip,NULL);
    uint64_t budget=long_timeout ? WPT_LONG_BUDGET_MS : WPT_BUDGET_MS, began=now_ms;
    char *text=NULL;
    for(;;) {
        if(!wpt_turns()) { out->why="the loop never came to rest"; break; }
        if((text=wpt_results_text())!=NULL) break;
        if(wpt_navigated) { out->why="the page navigated away"; break; }
        if(now_ms-began>=budget) {
            bool stopped=g.page.scripts!=NULL && !yonder_scripts_alive(g.page.scripts);
            out->why=stopped ? "its scripts were stopped (time or memory)" : "the harness never reported";
            break;
        }
        now_ms+=WPT_TICK_MS;
    }
    if(text!=NULL) {
        out->reported=true;
        for(char *save=NULL,*line=strtok_r(text,"\n",&save);line;line=strtok_r(NULL,"\n",&save))
            wpt_list_add(&out->lines,wpt_strdup(line));
        free(text);
    }
    while(wpt_answer()) {}
    stream_window_drop();
    check(nscript_jobs==0,"wpt: every script fetch was answered or cancelled");
    nscript_jobs=0;
    clock_step=0;
}

// ── The expectation file ──────────────────────────────────────────────────

typedef struct {
    wpt_list_t required, passing;   /* page<tab>test, sorted */
    wpt_list_t excluded;            /* page<tab>reason, sorted by page */
    /* The hand-written sections as they were written, comments and all,
     * for --update to copy. */
    wpt_list_t required_text, excluded_text;
} wpt_expect_t;

/* The file at `path` into `e`; a file that does not exist is three empty
 * sections. False, having said why, for one that exists and cannot be read
 * whole (a directory, past WPT_FILE_MAX, refused, read short), a line
 * outside any section, or an exclusion that gives no reason: a run against
 * expectations it could not read would judge nothing, and --update would
 * replace them. */
static bool wpt_expect_read(const char *path, wpt_expect_t *e) {
    memset(e,0,sizeof(*e));
    struct stat st;
    if(stat(path,&st)!=0) {
        if(errno==ENOENT) return true;
        fprintf(stderr,"wpt: %s: %s\n",path,strerror(errno));
        return false;
    }
    if(!S_ISREG(st.st_mode)) { fprintf(stderr,"wpt: %s is not a file\n",path); return false; }
    if((uint64_t)st.st_size>WPT_FILE_MAX) {
        fprintf(stderr,"wpt: %s is past the %u bytes an expectation file may hold\n",path,WPT_FILE_MAX);
        return false;
    }
    size_t len=0;
    char *text=wpt_slurp(path,&len);
    if(text==NULL) { fprintf(stderr,"wpt: %s could not be read whole\n",path); return false; }
    enum { NONE, REQUIRED, PASSING, EXCLUDED } section=NONE;
    bool ok=true;
    /* Every line, a blank one included: only what follows the last newline,
     * when it is empty, is no line at all. */
    for(char *line=text,*next;ok && line!=NULL;line=next) {
        next=strchr(line,'\n');
        if(next==NULL && *line=='\0') break;
        if(next!=NULL) *next++='\0';
        if(!strcmp(line,"[required]")) { section=REQUIRED; continue; }
        if(!strcmp(line,"[passing]")) { section=PASSING; continue; }
        if(!strcmp(line,"[excluded]")) { section=EXCLUDED; continue; }
        bool comment=line[0]=='#' || line[0]=='\0';
        if(section==REQUIRED) wpt_list_add(&e->required_text,wpt_strdup(line));
        if(section==EXCLUDED) wpt_list_add(&e->excluded_text,wpt_strdup(line));
        if(comment) continue;
        if(section==NONE) {
            fprintf(stderr,"wpt: %s: \"%s\" is in no section\n",path,line);
            ok=false;
        } else if(section==EXCLUDED && (strchr(line,'\t')==NULL || strchr(line,'\t')[1]=='\0')) {
            fprintf(stderr,"wpt: %s: the exclusion of %s gives no reason\n",path,line);
            ok=false;
        } else {
            wpt_list_add(section==REQUIRED ? &e->required : section==PASSING ? &e->passing : &e->excluded,
                wpt_strdup(line));
        }
    }
    free(text);
    wpt_list_settle(&e->required);
    wpt_list_settle(&e->passing);
    wpt_list_settle(&e->excluded);
    return ok;
}
static void wpt_expect_free(wpt_expect_t *e) {
    wpt_list_free(&e->required); wpt_list_free(&e->passing); wpt_list_free(&e->excluded);
    wpt_list_free(&e->required_text); wpt_list_free(&e->excluded_text);
}
/* The reason the expectations exclude `page` for, or NULL. */
static const char *wpt_excluded(const wpt_expect_t *e, const char *page) {
    size_t n=strlen(page);
    for(size_t i=0;i<e->excluded.n;i++)
        if(!strncmp(e->excluded.v[i],page,n) && e->excluded.v[i][n]=='\t') return e->excluded.v[i]+n+1;
    return NULL;
}
/* Whether `--only` selects the page an expectation line names. */
static bool wpt_selected(const char *line, const char *only) {
    if(only==NULL) return true;
    const char *tab=strchr(line,'\t');
    char *page=wpt_strndup(line,tab ? (size_t)(tab-line) : strlen(line));
    bool hit=strstr(page,only)!=NULL;
    free(page);
    return hit;
}
static bool wpt_has(const wpt_list_t *l, const char *s) {
    return l->n>0 && bsearch(&s,l->v,l->n,sizeof(*l->v),wpt_cmp)!=NULL;
}

/* [passing] from this run, the hand-written sections as they were. */
static int wpt_update(const char *expect, const wpt_expect_t *e, const wpt_list_t *passes, const char *only) {
    wpt_list_t kept={0};
    for(size_t i=0;i<e->passing.n;i++)
        if(!wpt_selected(e->passing.v[i],only)) wpt_list_add(&kept,wpt_strdup(e->passing.v[i]));
    for(size_t i=0;i<passes->n;i++) wpt_list_add(&kept,wpt_strdup(passes->v[i]));
    wpt_list_settle(&kept);
    /* The commit the checkout is at heads the file, so a moved pin shows in
     * the diff of every expectation written against it. */
    char *head_path=wpt_join(wpt_root,".git/HEAD"), *head=wpt_slurp(head_path,&(size_t){0});
    size_t hex=head!=NULL ? strspn(head,"0123456789abcdef") : 0;
    char *temp=wpt_fmt("%s.new",expect);
    FILE *f=fopen(temp,"w");
    int rc=0;
    if(f==NULL) {
        fprintf(stderr,"wpt: cannot write %s\n",temp); rc=2;
    } else {
        fprintf(f,"# WPT's %s in yonder, at WPT %.*s.\n"
            "# tools/test_yonder_wpt_host.c says what each section means. [required] and\n"
            "# [excluded] are written by hand; tools/test_yonder_wpt_host.sh %s --update\n"
            "# rewrites [passing].\n",
            wpt_dir,hex==40 ? 40 : 7,hex==40 ? head : "unknown",wpt_dir);
        fprintf(f,"[required]\n");
        for(size_t i=0;i<e->required_text.n;i++) fprintf(f,"%s\n",e->required_text.v[i]);
        fprintf(f,"[excluded]\n");
        for(size_t i=0;i<e->excluded_text.n;i++) fprintf(f,"%s\n",e->excluded_text.v[i]);
        fprintf(f,"[passing]\n");
        for(size_t i=0;i<kept.n;i++) fprintf(f,"%s\n",kept.v[i]);
        if(fclose(f)!=0 || rename(temp,expect)!=0) { fprintf(stderr,"wpt: cannot write %s\n",expect); rc=2; }
        else printf("wpt: %s lists %zu passes\n",expect,kept.n);
    }
    free(temp); free(head); free(head_path);
    wpt_list_free(&kept);
    return rc;
}

static int wpt_usage(void) {
    fprintf(stderr,"usage: test_yonder_wpt_host --wpt <checkout> --dir <dir> --expect <file>"
        " [--pages <file>] [--update] [--only <substring>] [--report <file>] [--modern-off] [--fault]"
        " [--short-read <path>]\n");
    return 2;
}

int main(int argc, char **argv) {
    const char *expect=NULL, *only=NULL, *pages=NULL;
    bool update=false;
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--update")) update=true;
        else if(i+1<argc && !strcmp(argv[i],"--wpt")) wpt_root=argv[++i];
        else if(i+1<argc && !strcmp(argv[i],"--dir")) wpt_dir=argv[++i];
        else if(i+1<argc && !strcmp(argv[i],"--expect")) expect=argv[++i];
        else if(i+1<argc && !strcmp(argv[i],"--pages")) pages=argv[++i];
        else if(i+1<argc && !strcmp(argv[i],"--only")) only=argv[++i];
        else if(i+1<argc && !strcmp(argv[i],"--report")) wpt_report=argv[++i];
        else if(!strcmp(argv[i],"--modern-off")) wpt_modern_off=true;
        else if(!strcmp(argv[i],"--fault")) wpt_fault=true;
        else if(i+1<argc && !strcmp(argv[i],"--short-read")) wpt_short_read=argv[++i];
        else return wpt_usage();
    }
    if(wpt_root==NULL || wpt_dir==NULL || expect==NULL) return wpt_usage();
    if(update && wpt_modern_off) {
        fprintf(stderr,"wpt: --update writes the switch on's baseline, and --modern-off turns it off\n");
        return 2;
    }
    char *top=wpt_join(wpt_root,wpt_dir);
    struct stat st;
    if(stat(top,&st)!=0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr,"wpt: %s is not in the checkout (tools/wpt_fetch.sh lists what is)\n",top);
        free(top);
        return 2;
    }
    wpt_list_t skipped={0};
    bool found=true;
    if(pages!=NULL) found=wpt_read_pages(pages,&skipped);
    else wpt_walk(top,"");
    free(top);
    wpt_expect_t e;
    bool judged=found && wpt_expect_read(expect,&e);
    for(size_t i=0;judged && i<e.required.n;i++) {
        char *page=wpt_strndup(e.required.v[i],strcspn(e.required.v[i],"\t"));
        judged=wpt_excluded(&e,page)==NULL;
        free(page);
        if(!judged) fprintf(stderr,"wpt: %s: %s is required of a page it excludes\n",expect,e.required.v[i]);
    }
    if(!judged) {
        if(found) wpt_expect_free(&e);
        wpt_pages_free();
        wpt_list_free(&skipped);
        return 2;
    }
    os64_text_options_t options={.memory={.alloc=probe_alloc,.free=probe_free},
        .backend=flow_test_backend(),.memory_cap=8*1024*1024};
    check(os64_text_create(&options,&probe_text)==OS64_FONT_OK,"text context");
    check(os64_text_font_bitmap(probe_text,&probe_font)==OS64_FONT_OK,"bitmap face");
    static const char *const kStatus[]={"PASS","FAIL","TIMEOUT","NOTRUN","PRECONDITION_FAILED"};
    unsigned counts[5]={0}, silent=0, harness_bad=0, ran=0, excluded=0, discounted=0, unjudged=0;
    wpt_list_t passes={0};
    for(size_t i=0;i<wpt_npages;i++) {
        const wpt_page_t *page=&wpt_pages[i];
        if(only!=NULL && strstr(page->name,only)==NULL) continue;
        const char *why=wpt_excluded(&e,page->name);
        if(why!=NULL) { excluded++; printf("EXCLUDED\t%s\t%s\n",page->name,why); continue; }
        ran++;
        unsigned faults=failures;
        wpt_outcome_t o;
        wpt_run(page,&o);
        if(failures!=faults) printf("FAULT\t%s\tthe window's fakes were asked for what they cannot do\n",page->name);
        if(o.unjudged) {
            unjudged++;
            printf("UNJUDGED\t%s\t%s\n",page->name,o.why);
            continue;
        }
        if(!o.reported) {
            silent++; counts[2]++;
            printf("TIMEOUT\t%s\t(no results: %s)\n",page->name,o.why);
        }
        /* The harness's verdict first: a page whose harness is not OK
         * passes nothing. */
        bool whole=true;
        for(size_t k=0;k<o.lines.n;k++)
            if(!strncmp(o.lines.v[k],"HARNESS\t",8) && strncmp(o.lines.v[k]+8,"OK\t",3)!=0) whole=false;
        for(size_t k=0;k<o.lines.n;k++) {
            char *line=o.lines.v[k], *rest=strchr(line,'\t');
            if(rest==NULL) continue;
            *rest++='\0';
            char *message=strchr(rest,'\t');
            if(message!=NULL) *message++='\0';
            if(!strcmp(line,"HARNESS")) {
                if(strcmp(rest,"OK")!=0) {
                    harness_bad++;
                    printf("HARNESS\t%s\t%s%s%s (the page fails whole)\n",page->name,rest,
                        message && *message ? ": " : "",message && *message ? message : "");
                }
                continue;
            }
            for(int s=0;s<5;s++) if(!strcmp(line,kStatus[s])) {
                if(s==0 && !whole) {
                    discounted++; counts[1]++;
                    printf("FAIL\t%s\t%s\tit passed, on a page whose harness did not end OK\n",page->name,rest);
                    continue;
                }
                printf("%s\t%s\t%s%s%s\n",kStatus[s],page->name,rest,
                    message && *message ? "\t" : "",message && *message ? message : "");
                counts[s]++;
                if(s==0) wpt_list_add(&passes,wpt_fmt("%s\t%s",page->name,rest));
            }
        }
        wpt_list_free(&o.lines);
    }
    wpt_list_settle(&passes);
    unsigned total=counts[0]+counts[1]+counts[2]+counts[3]+counts[4];
    printf("%s: %u pass, %u fail, %u timeout, %u notrun, %u precondition failed of %u, in %u pages"
        " (%u never reported; %u harness errors, failing whole, %u of the fails passes on those pages)\n",
        wpt_dir,counts[0],counts[1],counts[2],counts[3],counts[4],total,ran,silent,harness_bad,discounted);
    if(excluded>0) printf("%s: not run: %u pages the expectations exclude\n",wpt_dir,excluded);
    if(skipped.n>0) qsort(skipped.v,skipped.n,sizeof(*skipped.v),wpt_cmp);
    for(size_t i=0,j;i<skipped.n;i=j) {
        for(j=i;j<skipped.n && !strcmp(skipped.v[j],skipped.v[i]);j++) {}
        printf("%s: not run: %zu pages, %s\n",wpt_dir,j-i,skipped.v[i]);
    }
    int rc=0;
    if(wpt_modern_off) {
        /* The switch's proof: every required test is the surface's own, so
         * none may pass without it. The baseline is the switch on's, so it
         * is not compared. */
        unsigned leaked=0, gone=0;
        for(size_t i=0;i<e.required.n;i++) {
            if(!wpt_selected(e.required.v[i],only)) continue;
            if(wpt_has(&passes,e.required.v[i])) { printf("LEAKED\t%s\n",e.required.v[i]); leaked++; }
            else gone++;
        }
        printf("wpt: with the switch off, %u required tests do not pass and %u do\n",gone,leaked);
        if(leaked>0) { printf("wpt: %u required tests pass without the surface they require\n",leaked); rc=1; }
    } else {
        unsigned lost=0, gained=0, missing=0;
        for(size_t i=0;i<e.required.n;i++)
            if(wpt_selected(e.required.v[i],only) && !wpt_has(&passes,e.required.v[i])) {
                printf("REQUIRED\t%s\n",e.required.v[i]); missing++;
            }
        for(size_t i=0;i<e.passing.n;i++)
            if(wpt_selected(e.passing.v[i],only) && !wpt_has(&passes,e.passing.v[i])) {
                printf("LOST\t%s\n",e.passing.v[i]); lost++;
            }
        for(size_t i=0;i<passes.n;i++)
            if(!wpt_has(&e.passing,passes.v[i])) { printf("NEW\t%s\n",passes.v[i]); gained++; }
        if(missing>0) { printf("wpt: %u required tests did not pass\n",missing); rc=1; }
        if(!update && lost>0) { printf("wpt: %u passes lost against %s\n",lost,expect); rc=1; }
        else if(!update && gained>0) printf("wpt: %u new passes; --update adds them to %s\n",gained,expect);
    }
    if(wpt_fault) check(false,"wpt: the fault --fault asked for");
    wpt_list_free(&skipped);
    wpt_pages_free();
    os64_text_font_release(probe_font);
    check(os64_text_destroy(probe_text)==OS64_FONT_OK,"all layout text released");
    check(live==0,"native and engine heap is empty");
    /* Written last, and only by a run that can be judged, since the only
     * thing that may change the file is a page run to a verdict: a fault
     * anywhere, a page's or the teardown's, or a page never run (its source
     * unreadable, its navigation never started), leaves the expectations as
     * they were, and the run cannot be judged (exit 2). */
    if(unjudged!=0)
        fprintf(stderr,"wpt: %u pages were never run to a verdict%s\n",unjudged,
            update ? "; the expectations are not rewritten" : "");
    if(failures!=0)
        fprintf(stderr,"wpt: %u faults in the window's fakes%s\n",failures,
            update ? "; the expectations are not rewritten" : "");
    if(failures!=0 || unjudged!=0) {
        rc=2;
    } else if(update) {
        int written=wpt_update(expect,&e,&passes,only);
        if(written!=0) rc=written;
    }
    wpt_expect_free(&e);
    wpt_list_free(&passes);
    return rc;
}
