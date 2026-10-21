/* THE REPLAY RUNNER (MODERN_WEB.md rule 3, § Slice 0b, as built): a page
 * yonder kept beside its page file (yonder.conf's keep_pages, diag.h) fed
 * back to yonder's own window on the host, with no network, and its record
 * written again.
 *
 *     test_yonder_replay_host KEPT-DIR OUT-DIR [--seconds N] [--only TEXT]
 *     test_yonder_replay_host --self-test WORK-DIR
 *
 * ONLY A PAGE REPLAYED TO A VERDICT IS JUDGED AS THE PAGE: what each page
 * came to is one RpState, decided by rp_outcome alone from the record the
 * replay wrote, and every file this runner touches goes through
 * tools/test_yonder_replay_fs.c, whose faults become an UNJUDGED page or a
 * refused run, never an empty answer.
 *
 * A whole replay is written into an OUT-DIR with no records in it, and is
 * refused otherwise, since a record left from an earlier run would read as
 * that page's; `--only` replays the records whose names hold TEXT and may
 * add to one, saying how many it found there. An OUT-DIR or KEPT-DIR that
 * cannot be listed refuses the run before anything is written.
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
 *     it was where the page was kept, each line answering one job, and IN
 *     THE INDEX'S ORDER, which is the order the products reached the window
 *     where the page was kept: a job whose line comes later waits for it,
 *     so async scripts run in the order they ran there (rp_order), and a
 *     job still waiting when the replay ends is incomplete. One
 *     the index has no line for, or no line left for (asked more often
 *     than where it was kept), is not made up either way: it stays out, as
 *     if still on the wire, and the record gains `REPLAY incomplete <kind>
 *     <url>` (rp_answer). Pictures
 *     and fonts are never answered: they are not kept, and a page shows
 *     without them, as it does while they come;
 *   - THE CLOCK. A turn with nothing to do moves the window's milliseconds
 *     on by a tick (10 ms), until the page is quiet (nothing arriving,
 *     nothing out that will be answered, no timer set) or the virtual
 *     budget is spent (60 s, --seconds), so a page's timers fire as they
 *     would on the glass. The microsecond clock also moves 1 ms each time
 *     it is read, so a script that never ends is stopped at the script
 *     time limit as it is on the P5;
 *   - A NAVIGATION THE PAGE STARTS (a script setting `location`, a
 *     refresh, a form it sends) ends the replay there: the next page is
 *     not in this one's index, and a page left running would fire timers
 *     that never fired where it was kept. The record gains `REPLAY
 *     incomplete navigation <url>`;
 *   - a page that never arrived (its parse unfinished, its sheets awaited,
 *     or nothing at all when the budget was spent) or that asked for the
 *     next turn for ever gains `REPLAY unfinished <why>`, and
 *     yonder_corpus_diff.py calls it broken, never progress;
 *   - the page left as yonder leaves it when it closes, which is the
 *     departure write: that record, and its REPLAY lines after it, goes to
 *     OUT-DIR under the same name, and one line is printed with what the
 *     page came to (rp_outcome): `unfinished` or `incomplete` with the
 *     first REPLAY line, `clean` or the verdict's counts, or `unjudged`
 *     and why, and anything that went wrong in the harness itself (it did
 *     not let go of memory). A summary closes: `replayed P, clean C,
 *     incomplete I, unfinished U`, and how many records had no kept bytes
 *     (those are not kept pages, and are passed over).
 *
 * UNJUDGED, which makes the answer 2 and writes nothing for the page: a
 * `*.txt` that is not a whole record (rp_record_read) or cannot be read, a
 * record with a line that had no memory to be written (it may be missing
 * anything) or that says it was `kept: not whole`, a folder whose index is
 * missing, unreadable, of another format, damaged, or names a file that is
 * not there, and a replay whose record cannot be written. A record that
 * ran out of room for plain lines is judged: its head facts have room of
 * their own.
 *
 * --self-test is the proof (run by tools/test_yonder_replay_host.sh): the
 * fixture pages the scripted-page host's fixtures serve
 * (tools/yonder_loop_fixture, tools/yonder_script_fixture, tools/pages) and
 * hand-made ones are loaded with keep_pages on, as a person browsing would,
 * their sub-resources served by the runner; what yonder kept is written out
 * to WORK-DIR/kept beside the records it wrote, replayed into WORK-DIR/out
 * by the path above, and each replayed record must be the page's own line
 * for line, its paint stages aside (rp_same). Then copies of one, and
 * pages written here, are replayed as corpora that must not pass silently
 * (rp_corpus_cases).
 */
#define YONDER_HOST_FAKES_ONLY
#pragma GCC diagnostic ignored "-Wunused-function"
#include "test_yonder_scripts_host.c"

#include <ctype.h>
#include "test_yonder_replay_fs.c"

/* How far the clocks move: a tick when the loop has nothing to do, and the
 * microsecond clock at each read. */
#define RP_TICK_MS 10
#define RP_MICROS_STEP 1000
/* Turns in a row that each asked to come straight back: past this the page
 * is said to ring itself for ever, and is left. */
#define RP_BUSY_MAX 100000

/* `a`, `b` and `c` end to end: a path, malloc'd, however long. */
static char *rp_cat(const char *a, const char *b, const char *c) {
    size_t n=strlen(a)+strlen(b)+strlen(c)+1;
    char *s=malloc(n); snprintf(s,n,"%s%s%s",a,b,c);
    return s;
}

/* A field as a page file or an index wrote it (diag.c): `\\` and `\xHH`
 * read back, and, with `tokens`, a token whose second letter was
 * percent-encoded and a `%` written `%25`, in one pass from the left, which
 * gives back the data's bytes exactly. A new string. */
static char *rp_unescape(const char *s, size_t n, bool tokens) {
    char *out=malloc(n+1); size_t o=0;
    for(size_t i=0;i<n;) {
        if(s[i]=='\\' && i+1<n && s[i+1]=='\\') { out[o++]='\\'; i+=2; continue; }
        if(s[i]=='\\' && i+3<n && s[i+1]=='x' && isxdigit((unsigned char)s[i+2]) && isxdigit((unsigned char)s[i+3])) {
            char hex[3]={s[i+2],s[i+3],0}; out[o++]=(char)strtol(hex,NULL,16); i+=4; continue;
        }
        if(tokens && i+9<=n && !memcmp(s+i,"M%49SSING",9)) { memcpy(out+o,"MISSING",7); o+=7; i+=9; continue; }
        if(tokens && i+8<=n && !memcmp(s+i,"F%41ILED",8)) { memcpy(out+o,"FAILED",6); o+=6; i+=8; continue; }
        if(tokens && i+3<=n && !memcmp(s+i,"%25",3)) { out[o++]='%'; i+=3; continue; }
        out[o++]=s[i++];
    }
    out[o]='\0';
    return out;
}

/* ── What the record says ───────────────────────────────────────────── */

/* WHAT A PAGE CAME TO, in order of precedence: a record that says two is
 * the later, so a replay that reached no verdict is UNFINISHED whatever
 * else it met, as tools/yonder_records.py's State orders it. UNJUDGED is
 * the runner's own: no record of it is written. */
typedef enum { RP_VERDICT, RP_INCOMPLETE, RP_UNFINISHED, RP_UNJUDGED, RP_STATES } RpState;

enum { F_STATUS, F_FINAL, F_BODY, F_TYPE, F_CHARSET, F_ENCRYPTED, F_TLS, F_SCRIPTS, F_LAID, F_KEPT, F_N };
static const char *const kFacts[F_N]={"status","final address","body","content-type","charset","encrypted",
    "TLS","scripts","laid out at","kept"};
typedef struct {
    char *address;          /* line one, decoded */
    char *facts[F_N];       /* by kFacts, decoded; NULL when absent */
    unsigned missing, failed;   /* its record lines */
    RpState state;          /* by its REPLAY lines: RP_VERDICT for none */
    char *said;             /* the first REPLAY line of that state, after `REPLAY <state> `; NULL for none */
    unsigned incomplete;    /* its `REPLAY incomplete` lines */
    bool lost;              /* a line had no memory to be written */
} RpRecord;

static void rp_record_free(RpRecord *r) {
    free(r->address); free(r->said);
    for(int i=0;i<F_N;i++) free(r->facts[i]);
    memset(r,0,sizeof(*r));
}
/* A record's line `MISSING ... (<n>)` or `FAILED ... (<n>)`: it ends in a
 * count. */
static bool rp_counted(const char *line, size_t len) {
    if(len<4 || line[len-1]!=')') return false;
    size_t at=len-1;
    while(at>0 && isdigit((unsigned char)line[at-1])) at--;
    return at<len-1 && at>=2 && line[at-1]=='(' && line[at-2]==' ';
}
/* THE RECORD READ WHOLE, by the rules tools/yonder_records.py reads it by:
 * NULL with `r` filled, or why it is not a whole record (`r` empty). Its
 * first line `address: `, its second `verdict: clean` or the counts of the
 * tokens with records, its record lines exactly that many, each counted,
 * no other verdict line, no carriage return, and each `REPLAY` line one
 * of the states RpState names. */
static const char *rp_record_read(const char *text, RpRecord *r) {
    memset(r,0,sizeof(*r));
    if(strncmp(text,"address: ",9)!=0) return "it is not a page file: its first line is not `address: `";
    if(strchr(text,'\r')!=NULL) return "it is not a whole page file: a line ends in a carriage return";
    unsigned want_missing=0, want_failed=0, line_no=0;
    const char *why=NULL;
    for(const char *line=text; *line && why==NULL; line_no++) {
        const char *end=strchr(line,'\n'); size_t len=end ? (size_t)(end-line) : strlen(line);
        if(line_no==0) r->address=rp_unescape(line+9,len-9,true);
        else if(line_no==1) {
            char *v=strndup(line,len); unsigned a=0, b=0; int at=0;
            bool ok=!strcmp(v,"verdict: clean") ||
                (sscanf(v,"verdict: %u MISSING, %u FAILED%n",&a,&b,&at)==2 && v[at]=='\0' && (want_missing=a,want_failed=b,true)) ||
                (sscanf(v,"verdict: %u MISSING%n",&a,&at)==1 && v[at]=='\0' && (want_missing=a,true)) ||
                (sscanf(v,"verdict: %u FAILED%n",&a,&at)==1 && v[at]=='\0' && (want_failed=a,true));
            free(v);
            if(!ok) why="it is not a whole page file: its second line is not a verdict";
        } else if(!strncmp(line,"MISSING ",8) || !strncmp(line,"FAILED ",7)) {
            if(!rp_counted(line,len)) why="it is not a whole page file: a record line with no count";
            else if(line[0]=='M') r->missing++;
            else r->failed++;
        } else if(!strncmp(line,"verdict: ",9)) {
            why="it is not a whole page file: a second verdict line";
        } else if(!strncmp(line,"REPLAY ",7)) {
            RpState s=!strncmp(line,"REPLAY incomplete ",18) ? RP_INCOMPLETE :
                      !strncmp(line,"REPLAY unfinished ",18) ? RP_UNFINISHED : RP_STATES;
            if(s==RP_STATES) why="a REPLAY line of a state this runner does not know";
            else {
                r->incomplete+=s==RP_INCOMPLETE;
                if(s>r->state) { r->state=s; free(r->said); r->said=strndup(line+18,len-18); }
            }
        } else {
            if(len>31 && !strncmp(line+len-30,": no memory to write this line",30)) r->lost=true;
            for(int i=0;i<F_N;i++) {
                size_t k=strlen(kFacts[i]);
                if(r->facts[i]==NULL && len>k+1 && !strncmp(line,kFacts[i],k) && line[k]==':' && line[k+1]==' ')
                    r->facts[i]=rp_unescape(line+k+2,len-k-2,true);
            }
        }
        line+=len+(end ? 1 : 0);
    }
    if(why==NULL && line_no<2) why="it is not a whole page file: its second line is not a verdict";
    static char counts[160];
    if(why==NULL && (r->missing!=want_missing || r->failed!=want_failed)) {
        snprintf(counts,sizeof(counts),"it is not a whole page file: its verdict counts %u MISSING and %u FAILED "
                 "lines, and it holds %u and %u",want_missing,want_failed,r->missing,r->failed);
        why=counts;
    }
    if(why!=NULL) rp_record_free(r);
    return why;
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
 * runner answers, each with its file's bytes when it came, and how often
 * each was handed out, so the second job for one address is given the
 * second line kept for it. A job past the last has no line of its own: the
 * page asked more often than where it was kept, and an answer made up from
 * an earlier line would run a script or apply a sheet once more than the
 * page did, so it is unknown (incomplete). A kind it does not know is
 * passed over: a later slice's lines answer that slice's requests. */
typedef struct { uint32_t kind; bool came; char *n, *url, *final, *label; char *bytes; size_t len; unsigned used; } RpEntry;
typedef struct { char *folder; RpEntry *e; size_t n; } RpIndex;
/* An index line's number, which names its file: decimal digits and nothing
 * else, so it can never name a path outside the folder. */
static bool rp_number(const char *s) {
    size_t n=strspn(s,"0123456789");
    return n>0 && n<10 && s[n]=='\0';
}

static void rp_index_free(RpIndex *x) {
    for(size_t i=0;i<x->n;i++) { free(x->e[i].n); free(x->e[i].url); free(x->e[i].final); free(x->e[i].label); free(x->e[i].bytes); }
    free(x->e); free(x->folder); memset(x,0,sizeof(*x));
}
/* The folder's index, and the file of each line that came, read whole
 * before the replay begins: NULL, or why the page cannot be judged (its
 * reason in `fault` when the host said it). A page with NO FOLDER fetched
 * nothing it kept, and its index is empty; a folder whose index is not
 * there, cannot be read, names a format this runner does not read, has a
 * line whose number is not one (rp_number), or names a file that is not
 * there or cannot be read, is a kept page with a piece missing. */
static const char *rp_index_read(const char *folder, RpIndex *x, RpFault *fault) {
    memset(x,0,sizeof(*x));
    x->folder=strdup(folder);
    bool dir=false;
    RpFs got=rp_fs_kind(folder,&dir,fault);
    if(got==RP_FS_ABSENT) return NULL;
    if(got==RP_FS_FAULT) return fault->why;
    if(!dir) return "its folder is not a directory";
    char *path=rp_cat(folder,"/index","");
    char *text=NULL;
    got=rp_fs_read(path,&text,NULL,fault);
    free(path);
    if(got==RP_FS_ABSENT) return "its folder has no index";
    if(got==RP_FS_FAULT) return fault->why;
    char head[64]; snprintf(head,sizeof(head),"# yonder kept index %d\n",YONDER_DIAG_INDEX_VERSION);
    static char why[96];
    if(strncmp(text,head,strlen(head))) {
        free(text);
        snprintf(why,sizeof(why),"its index is not in the format this runner reads (# yonder kept index %d)",
                 YONDER_DIAG_INDEX_VERSION);
        return why;
    }
    const char *bad=NULL;
    for(char *line=text; bad==NULL && *line; ) {
        char *end=strchr(line,'\n'); if(end) *end='\0';
        char *f[6]={0}; int nf=0;
        for(char *p=line; nf<6; ) { f[nf++]=p; char *t=strchr(p,'\t'); if(!t) break; *t='\0'; p=t+1; }
        if(line[0]!='#' && !rp_number(f[0]))
            bad="its index is damaged: a line's number is not a decimal number";
        else if(line[0]!='#' && nf==6 && (!strcmp(f[1],"script") || !strcmp(f[1],"sheet"))) {
            x->e=realloc(x->e,(x->n+1)*sizeof(*x->e));
            RpEntry *e=&x->e[x->n++]; memset(e,0,sizeof(*e));
            e->kind=!strcmp(f[1],"script") ? YONDER_JOB_SCRIPT : YONDER_JOB_SHEET;
            e->n=strdup(f[0]);
            e->url=rp_unescape(f[2],strlen(f[2]),false);
            e->came=!strcmp(f[3],"came");
            e->final=rp_unescape(f[4],strlen(f[4]),false);
            e->label=rp_unescape(f[5],strlen(f[5]),false);
            if(e->came) {
                char *file=rp_cat(e->n,".",e->kind==YONDER_JOB_SCRIPT ? "js" : "css");
                char *at=rp_cat(folder,"/",file);
                got=rp_fs_read(at,&e->bytes,&e->len,fault);
                if(got==RP_FS_ABSENT) { snprintf(why,sizeof(why),"its index names %s, which is not there",file); bad=why; }
                else if(got==RP_FS_FAULT) bad=fault->why;
                free(at); free(file);
            }
        }
        if(!end) break;
        line=end+1;
    }
    free(text);
    return bad;
}
static RpKnown rp_index_lookup(void *ctx, uint32_t kind, const char *url, RpAnswer *out) {
    RpIndex *x=ctx; RpEntry *pick=NULL;
    for(size_t i=0;i<x->n && pick==NULL;i++)
        if(x->e[i].kind==kind && !strcmp(x->e[i].url,url) && x->e[i].used==0)
            pick=&x->e[i];
    if(pick==NULL) return RP_UNKNOWN;
    pick->used++;
    if(!pick->came) return RP_FAILED;
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
/* THE REPLAY'S OWN LINES, which the replayed record gains after what the
 * window wrote: `REPLAY incomplete <kind> <url>` for each thing the page
 * asked for that its source had nothing for (`incomplete` counts them),
 * and `REPLAY unfinished <why>` when the replay reached no verdict on the
 * page (`unfinished`). Both are neither token, so a grep for MISSING or
 * FAILED never meets them. */
static char *rp_lines; static size_t rp_lines_len;
static unsigned rp_incomplete;
static bool rp_unfinished;
/* A page from the network starts with a navigation job of the load's own;
 * any other is a navigation the page started itself, and the replay stops
 * there (`rp_navigated`). */
static bool rp_trip_expected, rp_navigated;

/* One REPLAY line: `head`, then `data` written as a page file writes data
 * (diag.c): a byte that would break the line, and a backslash, escaped, a
 * token with its second letter percent-encoded, and a `%` a reader would
 * take for part of a spelling as `%25`. */
static void rp_line(const char *head, const char *data) {
    size_t n=strlen(head)+strlen(data)*4+32;
    rp_lines=realloc(rp_lines,rp_lines_len+n);
    char *o=rp_lines+rp_lines_len;
    o+=sprintf(o,"REPLAY %s",head);
    for(const char *s=data; *s; s++) {
        unsigned char c=(unsigned char)*s;
        if(!strncmp(s,"MISSING",7) || !strncmp(s,"FAILED",6)) { o+=sprintf(o,"%c%%%02X",s[0],(unsigned char)s[1]); s++; }
        else if(c=='%' && (!strncmp(s+1,"25",2) || (s>data && s[-1]=='M' && !strncmp(s+1,"49SSING",7)) ||
                           (s>data && s[-1]=='F' && !strncmp(s+1,"41ILED",6)))) o+=sprintf(o,"%%25");
        else if(c=='\\') o+=sprintf(o,"\\\\");
        else if(c<0x20 || c==0x7F) o+=sprintf(o,"\\x%02X",c);
        else *o++=(char)c;
    }
    *o++='\n';
    rp_lines_len=(size_t)(o-rp_lines);
}
static void rp_incomplete_add(const char *kind, const char *url) {
    char head[64]; snprintf(head,sizeof(head),"incomplete %s ",kind);
    rp_line(head,url);
    rp_incomplete++;
}
static void rp_unfinished_add(const char *why) {
    rp_line("unfinished ",why);
    rp_unfinished=true;
}

static os64_work_id_t rp_submit(const os64_work_t *work) {
    if(rp_njobs==rp_capjobs) { rp_capjobs=rp_capjobs ? rp_capjobs*2 : 64; rp_jobs=realloc(rp_jobs,rp_capjobs*sizeof(*rp_jobs)); }
    RpJob *j=&rp_jobs[rp_njobs];
    *j=(RpJob){++rp_ids,*work,*(const uint32_t *)work->job,false};
    if(j->kind==YONDER_JOB_TRIP && rp_trip_expected) {
        rp_trip_expected=false;
    } else if(j->kind==YONDER_JOB_TRIP) {
        /* The page went somewhere: the next page is not in this one's
         * index (DEBTS.md § The replay corpus), so this one stops here. */
        j->unknown=true;
        rp_navigated=true;
        rp_incomplete_add("navigation",((const yonder_trip_t *)work->job)->url);
    }
    return rp_jobs[rp_njobs++].id;
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
/* The address a script's or a sheet's job carries. */
static const char *rp_job_url(const RpJob *j) {
    return j->kind==YONDER_JOB_SCRIPT ? ((const yonder_script_job_t *)j->work.job)->url
                                      : ((const yonder_sheet_job_t *)j->work.job)->url;
}
/* THE KEPT ORDER, when the answers come from a kept index (NULL when they
 * come from a self-test's own site, which has none): its lines are in the
 * order the products reached the window where the page was kept, and the
 * replay hands them over in that order, so two async scripts run in the
 * order they ran there whatever order the replay submitted them in. */
static RpIndex *rp_order;

/* The first index line no job has taken: the next the kept order hands
 * over. NULL when every line is taken. */
static const RpEntry *rp_next_line(void) {
    for(size_t i=0;rp_order!=NULL && i<rp_order->n;i++)
        if(rp_order->e[i].used==0) return &rp_order->e[i];
    return NULL;
}
/* Whether a line for this job is still to come in the kept order. */
static bool rp_line_left(const RpJob *j) {
    for(size_t i=0;i<rp_order->n;i++)
        if(rp_order->e[i].used==0 && rp_order->e[i].kind==j->kind && !strcmp(rp_order->e[i].url,rp_job_url(j)))
            return true;
    return false;
}
/* Whether job `j` may be answered now: a navigation's at once; a script's
 * or a sheet's when it is the job the kept order's next line names, or has
 * no line left at all (which makes it incomplete). */
static bool rp_turn_of(const RpJob *j) {
    if(j->kind==YONDER_JOB_TRIP || rp_order==NULL) return true;
    const RpEntry *head=rp_next_line();
    if(head!=NULL && head->kind==j->kind && !strcmp(head->url,rp_job_url(j))) return true;
    return !rp_line_left(j);
}
/* One round trip of the network: every job out when it began that the
 * replay answers and whose turn it is (rp_turn_of), in the order it was
 * submitted; a job whose line comes later waits for it. A job submitted
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
        while(i<rp_njobs && !(rp_jobs[i].id<=last && rp_answers(&rp_jobs[i]) && rp_turn_of(&rp_jobs[i]))) i++;
        if(i==rp_njobs) return;
        RpJob *j=&rp_jobs[i];
        void *product=NULL;
        if(j->kind!=YONDER_JOB_TRIP) {
            const char *url=rp_job_url(j);
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
/* The replay is over: a job still waiting for its line's turn never got
 * it (the line before it named a job this replay never asked for), so it
 * is incomplete, as one with no line is. A job whose turn it was and that
 * the budget ended before answering is not: the page is then unfinished,
 * and says so. */
static void rp_waiting_incomplete(void) {
    for(size_t i=0;i<rp_njobs;i++)
        if(rp_answers(&rp_jobs[i]) && rp_jobs[i].kind!=YONDER_JOB_TRIP && !rp_turn_of(&rp_jobs[i])) {
            rp_jobs[i].unknown=true;
            rp_incomplete_add(rp_jobs[i].kind==YONDER_JOB_SCRIPT ? "script" : "sheet",rp_job_url(&rp_jobs[i]));
        }
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
typedef enum { RP_QUIET, RP_SPENT, RP_BUSY, RP_NAVIGATED } RpEnd;
typedef struct { RpEnd end; bool arrived; unsigned harness_failures; size_t live_after; } RpRan;

static bool rp_quiet(void) {
    if(g.stream.active || g.coming.active || rp_answerable()) return false;
    return !yonder_scripts_alive(g.page.scripts) || yonder_scripts_timer_next(g.page.scripts)==UINT64_MAX;
}
/* The loop turned as main turns it, the clock moved a tick at a time when
 * a turn finds nothing to do, until the page is quiet, the budget is
 * spent, it asks for the next turn for ever, or it starts a navigation of
 * its own. A navigation to a file takes no job (open_local reads it at
 * once), so it shows as a load the replay did not start: the record
 * sequence (`next`, where it stood after the page's own load) moved. */
static RpEnd rp_settle(uint64_t budget_ms, uint32_t next) {
    uint64_t waited=0; unsigned busy=0;
    for(;;) {
        rp_answer();
        bool more=stream_turn();
        more=coming_turn() || more;
        more=script_turn() || more;
        pictures_settle();
        if(g.coming.active && yonder_now_ms()>=g.coming.due) { coming_show(); more=true; }
        if(!rp_navigated && s_diag.next!=next) {
            rp_navigated=true;
            rp_incomplete_add("navigation",g.stream.active ? g.stream.head.url : "file://");
        }
        if(rp_navigated)
            return RP_NAVIGATED;
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
 * the page left. The departure write is on the fake disk, in load->dir.
 * `order`: the kept index whose order the answers follow (rp_order), NULL
 * for a source with none. */
static RpRan rp_run(const RpLoad *load, rp_lookup_t lookup, void *ctx, RpIndex *order) {
    RpRan ran={0};
    unsigned failed=failures;
    rp_lookup=lookup; rp_lookup_ctx=ctx; rp_order=order;
    rp_lines_len=0; rp_incomplete=0; rp_unfinished=false;
    rp_trip_expected=!load->local; rp_navigated=false;
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
    ran.end=rp_settle(load->budget_ms,s_diag.next);
    rp_waiting_incomplete();
    ran.arrived=page_doc(&g.page)!=NULL;
    /* ONLY A PAGE REPLAYED TO A VERDICT IS JUDGED (yonder_corpus_diff.py):
     * what stopped short says why, on the record itself, so its lines are
     * never read as the page's. A page that went elsewhere has said so. */
    if(ran.end==RP_BUSY)
        rp_unfinished_add("it asked for the next turn for ever");
    else if(ran.end!=RP_NAVIGATED && !ran.arrived) {
        char why[96];
        snprintf(why,sizeof(why),"%s when the replay's %lu s were spent",
                 g.stream.active ? "its parse had not ended" : g.coming.active ? "it was still waiting for its sheets"
                                                                             : "it had not arrived",
                 (unsigned long)(load->budget_ms/1000));
        rp_unfinished_add(why);
    }
    /* Leaving, in yonder's own order as it closes: the pool first, which
     * lets go of what it still holds, then the pages, which write their
     * records. */
    clock_step=0;
    rp_release_all();
    pool_submit_hook=NULL; pool_cancel_hook=NULL; pool_open=false; g.pool=NULL; rp_order=NULL;
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

/* ── What a page came to ────────────────────────────────────────────── */

/* A page's outcome: its state, why (UNJUDGED's reason, or what the first
 * REPLAY line of its state says), its record lines, and its `REPLAY incomplete` lines,
 * which an UNFINISHED page may have as well. */
typedef struct {
    RpState state;
    char why[1100];
    unsigned missing, failed, incomplete;
} RpOutcome;

/* THE ONE PLACE A PAGE'S OUTCOME IS DECIDED: UNJUDGED when `unjudged`
 * names why, or when the record the replay wrote is not one rp_record_read
 * reads whole; otherwise the state its REPLAY lines say, with its record
 * lines counted. */
static RpOutcome rp_outcome(const char *unjudged, const char *record) {
    RpOutcome o; memset(&o,0,sizeof(o));
    RpRecord r;
    const char *bad=unjudged!=NULL ? unjudged : record==NULL ? "the replay wrote no record" : rp_record_read(record,&r);
    if(bad!=NULL) {
        o.state=RP_UNJUDGED;
        snprintf(o.why,sizeof(o.why),"%s%s",unjudged==NULL ? "the replay's own record: " : "",bad);
        return o;
    }
    o.state=r.state; o.missing=r.missing; o.failed=r.failed; o.incomplete=r.incomplete;
    snprintf(o.why,sizeof(o.why),"%s",r.said!=NULL ? r.said : "");
    rp_record_free(&r);
    return o;
}

/* ── Replaying a directory ──────────────────────────────────────────── */

/* What a directory's replay came to: how many pages came to each state,
 * how many of the judged were clean, and, beside them, the records with no
 * kept bytes, which are not kept pages and are passed over, and the pages
 * that did not let go of what they held or failed one of the harness's own
 * checks. An UNJUDGED page makes the run's answer 2, since a corpus with
 * pages nobody judged reads as one that passed. */
typedef struct { unsigned states[RP_STATES], clean, bare, leaked, broken; } RpTally;

/* The record's sequence, from its file name `<anything>-<digits>.txt`. */
static uint32_t rp_seq(const char *name) {
    size_t n=strlen(name);
    if(n<6 || strcmp(name+n-4,".txt")) return 0;
    size_t at=n-4; while(at>0 && isdigit((unsigned char)name[at-1])) at--;
    return at<n-4 && at>0 && name[at-1]=='-' ? (uint32_t)strtoul(name+at,NULL,10) : 0;
}

/* What the page came to, counted and printed on its line: the state, and
 * why or its counts. */
static void rp_report(const char *name, const RpOutcome *o, const RpRan *ran, size_t live_before, RpTally *t) {
    t->states[o->state]++;
    switch(o->state) {
    case RP_UNJUDGED:
        printf("%s  unjudged: %s\n",name,o->why);
        return;
    case RP_UNFINISHED:
        printf("%s  unfinished (%s)%s",name,o->why,o->incomplete ? ", incomplete" : "");
        break;
    case RP_INCOMPLETE: printf("%s  incomplete (%s)",name,o->why); break;
    case RP_VERDICT:
        if(o->missing==0 && o->failed==0) { t->clean++; printf("%s  clean",name); }
        else if(o->missing!=0 && o->failed!=0) printf("%s  %u MISSING, %u FAILED",name,o->missing,o->failed);
        else printf("%s  %u %s",name,o->missing ? o->missing : o->failed,o->missing ? "MISSING" : "FAILED");
        break;
    case RP_STATES: break;
    }
    if(ran->live_after!=live_before) { printf(" (%ld blocks not let go)",(long)(ran->live_after-live_before)); t->leaked++; }
    if(ran->harness_failures!=0) { printf(" (%u harness checks failed)",ran->harness_failures); t->broken++; }
    printf("\n");
}

/* The window's network head for a page, rebuilt from its record's facts. */
static void rp_head_of(const RpRecord *r, way_head_t *h) {
    if(r->facts[F_STATUS]!=NULL) {
        h->status=atoi(r->facts[F_STATUS]);
        const char *sp=strchr(r->facts[F_STATUS],' ');
        os64_strcopy(h->reason,sizeof(h->reason),sp!=NULL ? sp+1 : "");
    } else {
        h->status=200; os64_strcopy(h->reason,sizeof(h->reason),"OK");
    }
    os64_strcopy(h->url,sizeof(h->url),r->facts[F_FINAL]!=NULL ? r->facts[F_FINAL] : r->address);
    os64_strcopy(h->content_type,sizeof(h->content_type),r->facts[F_TYPE]!=NULL ? r->facts[F_TYPE] : "");
    os64_strcopy(h->charset,sizeof(h->charset),r->facts[F_CHARSET]!=NULL ? r->facts[F_CHARSET] : "");
    h->encrypted=r->facts[F_ENCRYPTED]!=NULL && !strcmp(r->facts[F_ENCRYPTED],"yes");
    const char *body=r->facts[F_BODY]!=NULL ? r->facts[F_BODY] : "html";
    h->body=!strcmp(body,"text") ? WAY_BODY_TEXT : !strcmp(body,"image") ? WAY_BODY_IMAGE : WAY_BODY_HTML;
    if(r->facts[F_TLS]!=NULL) {
        h->tls_version=!strncmp(r->facts[F_TLS],"1.3",3) ? 0x0304 : 0x0303;
        h->tls_fallback=strstr(r->facts[F_TLS],"fallback")!=NULL;
    }
}

/* One kept page replayed into `out`: what it came to, decided by
 * rp_outcome and reported by rp_report, and its record written only when
 * it is judged. */
static void rp_replay_one(const char *kept, const char *out, const char *name, uint64_t budget_ms, RpTally *t) {
    char *stem=strndup(name,strlen(name)-4);
    char *base=rp_cat(kept,"/",stem);
    free(stem);
    RpFault fault;
    RpRecord r; memset(&r,0,sizeof(r));
    RpIndex index; memset(&index,0,sizeof(index));
    RpRan ran; memset(&ran,0,sizeof(ran));
    char *text=NULL, *bytes=NULL, *whole=NULL;
    const char *unjudged=NULL;
    size_t len=0, live_before=live;
    char *path=rp_cat(base,".txt","");
    RpFs got=rp_fs_read(path,&text,NULL,&fault);
    free(path);
    if(got!=RP_FS_OK) { unjudged=got==RP_FS_FAULT ? fault.why : "it is gone"; goto judge; }
    if((unjudged=rp_record_read(text,&r))!=NULL) goto judge;
    path=rp_cat(base,".html","");
    got=rp_fs_read(path,&bytes,&len,&fault);
    free(path);
    /* A record with no kept bytes is not a kept page: keep_pages was off
     * when it was written. */
    if(got==RP_FS_ABSENT) { t->bare++; goto done; }
    if(got==RP_FS_FAULT) { unjudged=fault.why; goto judge; }
    /* A kept page whose record says a line had no memory to be written may
     * be missing anything, the facts its head is rebuilt from included. One
     * that ran out of ROOM for plain lines is judged: the head facts have
     * room of their own (yonder_diag_head_fact), and a replay is compared
     * with another made under the same caps, which drop the same lines. */
    if(r.lost) { unjudged="a line of its record had no memory to be written"; goto judge; }
    if(r.facts[F_KEPT]!=NULL) {
        static char not_whole[512];
        snprintf(not_whole,sizeof(not_whole),"it was kept %s",r.facts[F_KEPT]);
        unjudged=not_whole; goto judge;
    }
    path=rp_cat(base,".d","");
    unjudged=rp_index_read(path,&index,&fault);
    free(path);
    if(unjudged!=NULL) goto judge;
    RpLoad load={0};
    load.address=r.address; load.seq=rp_seq(name); load.bytes=(const uint8_t *)bytes; load.len=len;
    load.local=!strncmp(r.address,"file://",7);
    load.scripts=r.facts[F_SCRIPTS]==NULL || strcmp(r.facts[F_SCRIPTS],"off")!=0;
    load.width=r.facts[F_LAID]!=NULL ? atoi(r.facts[F_LAID]) : 0;
    load.dir="/tmp/replay"; load.budget_ms=budget_ms;
    rp_head_of(&r,&load.head);
    fake_reset(); snprintf(fake_dirs[nfake_dirs++],256,"/tmp/replay");
    ran=rp_run(&load,rp_index_lookup,&index,&index);
    char written[256]; size_t wlen=0;
    const char *record=rp_written("/tmp/replay",r.address,load.seq,written,sizeof(written),&wlen);
    /* The record as the window wrote it, then the replay's own lines. */
    if(record!=NULL) {
        whole=malloc(wlen+rp_lines_len+1);
        memcpy(whole,record,wlen);
        if(rp_lines_len) memcpy(whole+wlen,rp_lines,rp_lines_len);
        whole[wlen+rp_lines_len]='\0';
    }
    fake_reset();
judge:;
    RpOutcome o=rp_outcome(unjudged,whole);
    if(o.state!=RP_UNJUDGED) {
        path=rp_cat(out,"/",name);
        if(rp_fs_write(path,whole,strlen(whole),&fault)!=RP_FS_OK)
            o=rp_outcome(fault.why,NULL);
        free(path);
    }
    rp_report(name,&o,&ran,live_before,t);
done:
    free(whole); free(text); free(bytes); free(base);
    rp_index_free(&index); rp_record_free(&r);
}

/* A whole replay (no `only`) is written into an OUT-DIR that holds no
 * records yet, and refused otherwise: a record left from an earlier run,
 * for a page since removed or no longer replayable, would be read by the
 * diff as that page's. A partial one (`only`) may add to one, and says how
 * many records there were before it. Either is refused before anything is
 * written when OUT-DIR or KEPT-DIR cannot be listed, or OUT-DIR made. */
static int rp_replay_dir(const char *kept, const char *out, uint64_t budget_ms, const char *only, RpTally *t) {
    RpFault fault;
    char **held=NULL, **names=NULL; size_t before=0, n=0;
    RpFs got=rp_fs_list(out,".txt",&held,&before,&fault);
    rp_fs_names_free(held,before);
    if(got==RP_FS_FAULT) { fprintf(stderr,"replay: %s; nothing was replayed\n",fault.why); return 2; }
    if(only==NULL && before!=0) {
        fprintf(stderr,"replay: %s already holds %zu records; a whole replay is written into an empty "
                       "or new directory\n",out,before);
        return 2;
    }
    got=rp_fs_list(kept,".txt",&names,&n,&fault);
    if(got!=RP_FS_OK) {
        fprintf(stderr,"replay: %s\n",got==RP_FS_FAULT ? fault.why : "the kept directory is not there");
        return 2;
    }
    if(rp_fs_mkdir(out,&fault)!=RP_FS_OK) {
        rp_fs_names_free(names,n);
        fprintf(stderr,"replay: %s; nothing was replayed\n",fault.why);
        return 2;
    }
    for(size_t i=0;i<n;i++)
        if(only==NULL || strstr(names[i],only)!=NULL) { rp_replay_one(kept,out,names[i],budget_ms,t); fflush(stdout); }
    rp_fs_names_free(names,n);
    unsigned replayed=t->states[RP_VERDICT]+t->states[RP_INCOMPLETE]+t->states[RP_UNFINISHED];
    printf("replayed %u, clean %u, incomplete %u, unfinished %u",replayed,t->clean,t->states[RP_INCOMPLETE],
           t->states[RP_UNFINISHED]);
    if(t->bare) printf("; %u records carry no kept bytes",t->bare);
    if(t->states[RP_UNJUDGED]) printf("; %u unjudged",t->states[RP_UNJUDGED]);
    if(t->leaked) printf("; %u did not let go of what they held",t->leaked);
    if(t->broken) printf("; %u broke the harness",t->broken);
    if(only!=NULL)
        printf("; a partial run (--only %s): %s held %zu records before it, which stand except those it "
               "wrote again",only,out,before);
    printf("\n");
    return t->states[RP_UNJUDGED] ? 2 : t->leaked || t->broken ? 1 : 0;
}

/* ── The self-test ──────────────────────────────────────────────────── */

/* The self-test's own files go through the same door, and a fault is said
 * and counted as a problem: NULL from rp_file when the file is not there
 * or could not be read. */
static char *rp_file(const char *path, size_t *len) {
    RpFault fault; char *bytes=NULL;
    RpFs got=rp_fs_read(path,&bytes,len,&fault);
    if(got==RP_FS_FAULT) printf("  %s\n",fault.why);
    return got==RP_FS_OK ? bytes : NULL;
}
static bool rp_put(const char *path, const void *bytes, size_t len) {
    RpFault fault;
    if(rp_fs_write(path,bytes,len,&fault)==RP_FS_OK) return true;
    printf("  %s\n",fault.why);
    return false;
}
static bool rp_put_dir(const char *path) {
    RpFault fault;
    if(rp_fs_mkdir(path,&fault)==RP_FS_OK) return true;
    printf("  %s\n",fault.why);
    return false;
}
static unsigned rp_replayed(const RpTally *t) {
    return t->states[RP_VERDICT]+t->states[RP_INCOMPLETE]+t->states[RP_UNFINISHED];
}

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
    free(file); file=rp_file(path,&len);
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
            if(pass==0 && slash!=NULL) { *slash='\0'; ok=rp_put_dir(path) && ok; }
            if(pass==1) ok=rp_put(path,fake_files[f].data,fake_files[f].len) && ok;
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

/* THE CASES A CORPUS MUST NOT PASS SILENTLY, each a copy of the first
 * hand-made page (or a page written here) in a folder of its own under
 * WORK-DIR, replayed as a corpus is. */
typedef enum { RP_COPY, RP_DROP_SCRIPT_LINE, RP_BAD_NUMBER, RP_NO_MEMORY, RP_FULL_TABLE } RpEdit;

/* The kept page `name` copied from `kept` into `dir`, with `edit` made to
 * it. The problems found. */
static unsigned rp_copy_page(const char *kept, const char *name, const char *dir, RpEdit edit) {
    unsigned bad=0;
    char *stem=strndup(name,strlen(name)-4);
    char *from=rp_cat(kept,"/",stem), *to=rp_cat(dir,"/",stem);
    char *folder=rp_cat(to,".d","");
    if(!rp_put_dir(dir) || !rp_put_dir(folder)) bad++;
    static const char *const kFiles[]={".txt",".html",".d/1.js",".d/2.css",".d/3.css",".d/index"};
    for(size_t f=0;f<sizeof(kFiles)/sizeof(kFiles[0]);f++) {
        char *a=rp_cat(from,kFiles[f],""), *b=rp_cat(to,kFiles[f],"");
        size_t len=0; char *text=rp_file(a,&len);
        if(text==NULL) { printf("  %s: %s%s was not kept\n",dir,stem,kFiles[f]); bad++; }
        else {
            if(!strcmp(kFiles[f],".d/index") && edit==RP_DROP_SCRIPT_LINE) {
                char *line=strstr(text,"\tscript\thttps://fixture.test/m.js\t");
                char *start=line; while(start!=NULL && start>text && start[-1]!='\n') start--;
                char *end=line!=NULL ? strchr(line,'\n') : NULL;
                if(end==NULL) { printf("  %s: the index has no line for m.js\n",dir); bad++; }
                else { memmove(start,end+1,strlen(end+1)+1); len=strlen(text); }
            }
            if(!strcmp(kFiles[f],".d/index") && edit==RP_BAD_NUMBER) {
                char *line=strstr(text,"\n1\t");
                if(line==NULL) { printf("  %s: the index has no line 1\n",dir); bad++; }
                else {
                    size_t at=(size_t)(line+1-text);
                    text=realloc(text,len+4);
                    memmove(text+at+3,text+at,len-at+1); memcpy(text+at,"../",3); len+=3;
                }
            }
            if(!strcmp(kFiles[f],".txt") && (edit==RP_NO_MEMORY || edit==RP_FULL_TABLE)) {
                const char *add=edit==RP_NO_MEMORY ? "written: no memory to write this line\n" : "facts not kept: 3\n";
                text=realloc(text,len+strlen(add)+1); memcpy(text+len,add,strlen(add)+1); len+=strlen(add);
            }
            if(!rp_put(b,text,len)) bad++;
        }
        free(text); free(a); free(b);
    }
    free(folder); free(from); free(to); free(stem);
    return bad;
}

/* `dir` replayed into `<dir>-out`: its answer, its tally, and the record
 * written for `name` (malloc'd, NULL for none). */
static int rp_replay_copy(const char *dir, const char *name, uint64_t budget_ms, RpTally *t, char **record) {
    char *out=rp_cat(dir,"-out","");
    memset(t,0,sizeof(*t));
    int rc=rp_replay_dir(dir,out,budget_ms,NULL,t);
    char *got=rp_cat(out,"/",name);
    *record=rp_file(got,NULL);
    free(got); free(out);
    return rc;
}

static unsigned rp_corpus_cases(const char *work, const char *kept, const char *name) {
    unsigned bad=0;
    RpTally t; char *record; int rc;
    char *dir;

    /* A request the index has no line for is not made up, neither as
     * content nor as a failure: the record gains `REPLAY incomplete script
     * <url>` and no FAILED line for it. The script blocked the parse, so
     * the page did not arrive either, and says that too: it is UNFINISHED,
     * the later state. */
    dir=rp_cat(work,"/partial","");
    bad+=rp_copy_page(kept,name,dir,RP_DROP_SCRIPT_LINE);
    rc=rp_replay_copy(dir,name,60000,&t,&record);
    if(rc!=0 || rp_replayed(&t)!=1 || t.states[RP_UNFINISHED]!=1 || t.clean!=0 || record==NULL ||
       strstr(record,"\nREPLAY incomplete script https://fixture.test/m.js\n")==NULL ||
       strstr(record,"\nREPLAY unfinished its parse had not ended when the replay's 60 s were spent\n")==NULL ||
       strstr(record,"FAILED script https://fixture.test/m.js")!=NULL) {
        printf("  a request the index lacks did not read as incomplete\n"); bad++;
    }
    free(record); free(dir);

    /* An index line whose number is not one, a record with a line that had
     * no memory to be written, and a `*.txt` that is not a record: none is
     * judged, and each makes the answer 2. */
    static const struct { const char *dir; RpEdit edit; } kRefused[]={
        {"/badnumber",RP_BAD_NUMBER},{"/nomemory",RP_NO_MEMORY},{"/notarecord",RP_COPY}};
    for(size_t i=0;i<sizeof(kRefused)/sizeof(kRefused[0]);i++) {
        dir=rp_cat(work,kRefused[i].dir,"");
        bad+=rp_copy_page(kept,name,dir,kRefused[i].edit);
        if(kRefused[i].edit==RP_COPY) {
            char *notes=rp_cat(dir,"/notes.txt","");
            if(!rp_put(notes,"not a page file\n",16)) bad++;
            free(notes);
        }
        rc=rp_replay_copy(dir,name,60000,&t,&record);
        unsigned judged=kRefused[i].edit==RP_COPY ? 1 : 0;
        if(rc!=2 || t.states[RP_UNJUDGED]!=1 || rp_replayed(&t)!=judged) {
            printf("  %s: a corpus with a page nobody can judge answered %d\n",kRefused[i].dir,rc); bad++;
        }
        free(record); free(dir);
    }

    /* A record that ran out of room for plain lines is judged: its head
     * facts had room of their own. */
    dir=rp_cat(work,"/fulltable","");
    bad+=rp_copy_page(kept,name,dir,RP_FULL_TABLE);
    rc=rp_replay_copy(dir,name,60000,&t,&record);
    if(rc!=0 || t.states[RP_UNJUDGED]!=0 || rp_replayed(&t)!=1) {
        printf("  a record whose table was full was not judged (%d)\n",rc); bad++;
    }
    free(record); free(dir);

    /* No time to finish: every page that did not arrive says so on its
     * record, and none is clean. */
    dir=rp_cat(work,"/zero","");
    bad+=rp_copy_page(kept,name,dir,RP_COPY);
    rc=rp_replay_copy(dir,name,0,&t,&record);
    if(rc!=0 || t.states[RP_UNFINISHED]!=1 || t.clean!=0 || record==NULL || strstr(record,"\nREPLAY unfinished ")==NULL) {
        printf("  a page given no time did not read as unfinished\n"); bad++;
    }
    free(record); free(dir);

    /* A page that asks for one address more often than where it was kept:
     * the extra job has no line of its own, so it is not answered from the
     * first one again (which would run the script twice); it is
     * incomplete, and, since it blocks the parse, the page is unfinished.
     * Then a whole replay into an OUT-DIR that already holds records is
     * refused, and a partial one adds to it. */
    dir=rp_cat(work,"/twice","");
    char *dfolder=rp_cat(dir,"/fixture.test-0001.d","");
    if(!rp_put_dir(dir) || !rp_put_dir(dfolder)) bad++;
    static const char kTwiceRecord[]="address: http://fixture.test/twice\nverdict: clean\nstatus: 200 OK\n"
        "final address: http://fixture.test/twice\nbody: html\ncontent-type: text/html\nencrypted: no\n";
    static const char kTwicePage[]="<!doctype html><p>twice</p><script src=t.js></script><script src=t.js></script>";
    static const char kTwiceIndex[]="# yonder kept index 1\n1\tscript\thttp://fixture.test/t.js\tcame\thttp://fixture.test/t.js\t\n";
    char *f1=rp_cat(dir,"/fixture.test-0001.txt",""), *f2=rp_cat(dir,"/fixture.test-0001.html","");
    char *f3=rp_cat(dfolder,"/index",""), *f4=rp_cat(dfolder,"/1.js","");
    if(!rp_put(f1,kTwiceRecord,sizeof(kTwiceRecord)-1) || !rp_put(f2,kTwicePage,sizeof(kTwicePage)-1) ||
       !rp_put(f3,kTwiceIndex,sizeof(kTwiceIndex)-1) || !rp_put(f4,"1",1)) bad++;
    rc=rp_replay_copy(dir,"fixture.test-0001.txt",60000,&t,&record);
    if(rc!=0 || t.states[RP_UNFINISHED]!=1 || t.clean!=0 || record==NULL ||
       strstr(record,"\nREPLAY incomplete script http://fixture.test/t.js\n")==NULL) {
        printf("  a script asked for twice and kept once was answered twice\n"); bad++;
    }
    free(record);
    char *again=rp_cat(dir,"-out","");
    RpTally t2={0};
    if(rp_replay_dir(dir,again,60000,NULL,&t2)!=2 || rp_replayed(&t2)!=0) {
        printf("  a whole replay into a directory holding records was not refused\n"); bad++;
    }
    memset(&t2,0,sizeof(t2));
    if(rp_replay_dir(dir,again,60000,"fixture",&t2)!=0 || rp_replayed(&t2)!=1) {
        printf("  a partial replay into a directory holding records was refused\n"); bad++;
    }
    free(again); free(f1); free(f2); free(f3); free(f4); free(dfolder); free(dir);

    /* Two async scripts that came back in the reverse of the order they
     * were asked for: the replay hands them over as they came where the
     * page was kept, and the page sees that order. */
    dir=rp_cat(work,"/order","");
    dfolder=rp_cat(dir,"/fixture.test-0001.d","");
    if(!rp_put_dir(dir) || !rp_put_dir(dfolder)) bad++;
    static const char kOrderRecord[]="address: http://fixture.test/order\nverdict: clean\nstatus: 200 OK\n"
        "final address: http://fixture.test/order\nbody: html\ncontent-type: text/html\nencrypted: no\n";
    static const char kOrderPage[]="<!doctype html><script>var order=[];</script>"
        "<script async src=a.js></script><script async src=b.js></script>"
        "<script>setTimeout(function(){if(order.join()!='b,a')wrongOrder()},1000)</script>";
    static const char kOrderIndex[]="# yonder kept index 1\n"
        "1\tscript\thttp://fixture.test/b.js\tcame\thttp://fixture.test/b.js\t\n"
        "2\tscript\thttp://fixture.test/a.js\tcame\thttp://fixture.test/a.js\t\n";
    f1=rp_cat(dir,"/fixture.test-0001.txt",""); f2=rp_cat(dir,"/fixture.test-0001.html","");
    f3=rp_cat(dfolder,"/index",""); f4=rp_cat(dfolder,"/1.js","");
    char *f5=rp_cat(dfolder,"/2.js","");
    if(!rp_put(f1,kOrderRecord,sizeof(kOrderRecord)-1) || !rp_put(f2,kOrderPage,sizeof(kOrderPage)-1) ||
       !rp_put(f3,kOrderIndex,sizeof(kOrderIndex)-1) || !rp_put(f4,"order.push('b')",15) ||
       !rp_put(f5,"order.push('a')",15)) bad++;
    rc=rp_replay_copy(dir,"fixture.test-0001.txt",60000,&t,&record);
    if(rc!=0 || t.clean!=1 || t.states[RP_INCOMPLETE]!=0 || t.states[RP_UNFINISHED]!=0 || record==NULL || strstr(record,"wrongOrder")!=NULL) {
        printf("  two async scripts did not run in the order they were kept in\n"); bad++;
    }
    free(record); free(f1); free(f2); free(f3); free(f4); free(f5); free(dfolder); free(dir);

    /* A page that goes somewhere of its own: the replay stops there, says
     * where it went, and the timer the page set for later never fires. */
    dir=rp_cat(work,"/navigates","");
    if(!rp_put_dir(dir)) bad++;
    static const char kRecord[]="address: http://fixture.test/goes\nverdict: clean\nstatus: 200 OK\n"
        "final address: http://fixture.test/goes\nbody: html\ncontent-type: text/html\nencrypted: no\n";
    static const char kPage[]="<!doctype html><p>going</p><script>"
        "setTimeout(function(){location.href='http://fixture.test/next'},100);"
        "setTimeout(function(){tooLate()},5000)</script>";
    char *txt=rp_cat(dir,"/fixture.test-0001.txt",""), *html=rp_cat(dir,"/fixture.test-0001.html","");
    if(!rp_put(txt,kRecord,sizeof(kRecord)-1) || !rp_put(html,kPage,sizeof(kPage)-1)) bad++;
    rc=rp_replay_copy(dir,"fixture.test-0001.txt",60000,&t,&record);
    if(rc!=0 || t.states[RP_INCOMPLETE]!=1 || t.clean!=0 || record==NULL ||
       strstr(record,"\nREPLAY incomplete navigation http://fixture.test/next\n")==NULL ||
       strstr(record,"tooLate")!=NULL) {
        printf("  a page that navigated was not stopped there\n"); bad++;
    }
    free(record); free(txt); free(html); free(dir);
    return bad;
}

static int rp_self_test(const char *work) {
    char kept[4096], out[4096];
    snprintf(kept,sizeof(kept),"%s/kept",work); snprintf(out,sizeof(out),"%s/out",work);
    if(!rp_put_dir(kept) || !rp_put_dir(out)) {
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
        {"http://fixture.test/M%49SSING/MISSING/s.js","document.body.className='came'",NULL,NULL},
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
        /* An address holding `M%49SSING` of its own beside MISSING: the
         * record writes them apart, and the replay must ask for the script
         * by the same address the page did. */
        {"an address that spells a token both ways","http://fixture.test/M%49SSING/MISSING/p.html",
         "<!doctype html><p>percent</p><script src=s.js></script>",
         rp_head(200,"OK","text/html","","http://fixture.test/M%49SSING/MISSING/p.html",false,WAY_BODY_HTML),
         NULL,"M%2549SSING/M%49SSING/s.js: ran",0,false},
    };
    const char *dirs[]={"tools/yonder_loop_fixture","tools/yonder_script_fixture","tools/pages"};
    unsigned pages=0, bad=0;
    uint32_t seq=0;
    char name[256], first_hand[256]="";
    /* Each page loaded as a person browsing with keep_pages on would load
     * it, and what yonder kept written out. */
    for(int set=0;set<4;set++) {
        char **files=NULL; size_t nfiles=0;
        RpFault fault;
        if(set<3 && rp_fs_list(dirs[set],".html",&files,&nfiles,&fault)!=RP_FS_OK) {
            printf("  cannot list %s\n",dirs[set]); bad++; continue;
        }
        size_t count=set<3 ? nfiles : sizeof(hand)/sizeof(hand[0]);
        for(size_t i=0;i<count;i++) {
            char address[512], *file=NULL; size_t len=0;
            RpLoad load={0};
            RpSite site={set<3 ? dirs[set] : NULL,kServed};
            if(set<3) {
                char *path=rp_cat(dirs[set],"/",files[i]);
                file=rp_file(path,&len);
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
            RpRan ran=rp_run(&load,rp_site_lookup,&site,NULL);
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
        }
        rp_fs_names_free(files,nfiles);
    }
    /* Every one replayed from what was written out, the way a corpus is. */
    RpTally t={0};
    int rc=rp_replay_dir(kept,out,60000,NULL,&t);
    if(rc!=0 || rp_replayed(&t)!=pages || t.states[RP_INCOMPLETE]!=0 || t.states[RP_UNFINISHED]!=0) bad++;
    char **records=NULL; size_t nrecords=0;
    RpFault fault;
    if(rp_fs_list(kept,".txt",&records,&nrecords,&fault)!=RP_FS_OK || nrecords!=pages) bad++;
    for(size_t i=0;i<nrecords;i++) {
        size_t alen=0, blen=0;
        char *a=rp_cat(kept,"/",records[i]), *b=rp_cat(out,"/",records[i]);
        char *x=rp_file(a,&alen), *y=rp_file(b,&blen);
        if(x==NULL || y==NULL || !rp_same(records[i],x,y)) bad++;
        free(x); free(y); free(a); free(b);
    }
    rp_fs_names_free(records,nrecords);
    bad+=rp_corpus_cases(work,kept,first_hand);
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
