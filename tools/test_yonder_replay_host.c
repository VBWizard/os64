/* THE REPLAY RUNNER (MODERN_WEB.md rule 3, § Slice 0b, as built): a page
 * yonder kept beside its page file (yonder.conf's keep_pages, diag.h) fed
 * back to yonder's own window on the host, with no network, and its record
 * written again.
 *
 *     test_yonder_replay_host KEPT-DIR OUT-DIR [--seconds N] [--only TEXT]
 *     test_yonder_replay_host --self-test WORK-DIR
 *
 * ONLY A PAGE REPLAYED TO A VERDICT IS JUDGED AS THE PAGE, AND ITS STATE IS
 * IN ITS RECORD, WHOLE OR ABSENT: what each page came to is one RpState,
 * decided by rp_decide alone from the page's own run, written as its
 * record's second line, `REPLAY state: <state> <why>`, and the record ends
 * with `end`, which every reader requires; every file this runner touches
 * goes through test_yonder_replay_fs.h, whose writes are whole or not at
 * all and whose faults become an UNJUDGED page or a refused run, never an
 * empty answer, and the host's file functions are poisoned below it.
 *
 * A whole replay is written into an OUT-DIR with no records in it, and is
 * refused otherwise, since a record left from an earlier run would read as
 * that page's; `--only` replays the records whose names hold TEXT and may
 * add to one, saying how many it found there, and a page it finds no
 * longer kept replaces its earlier record with one saying it is unjudged.
 * An OUT-DIR or KEPT-DIR that cannot be listed, or an OUT-DIR that is the
 * KEPT-DIR, refuses the run before anything is written.
 *
 * The window and every fake are tools/test_yonder_scripts_host.c's, the
 * headless binding: this file includes it for them and brings its own main.
 * For each `<name>.txt` in KEPT-DIR whose record says `kept: whole` (yonder
 * writes it once the page's bytes are kept whole; a record without it is
 * not a kept page, and is passed over):
 *
 *   - a window as wide as the record says the page was laid out, scripts
 *     on unless the record says `scripts: off`, the script time limit the
 *     default (the record does not say what it was), and a text context of
 *     the page's own;
 *   - its kept bytes and index checked against what its record says of
 *     them (`bytes: N`, `kept lines: N`), since nothing else on the host
 *     binds a record to its kept files;
 *   - start_trip to the address on the record's first line. A page from the
 *     network gets a head rebuilt from the record's facts (status, final
 *     address, body, content-type, charset, encrypted, TLS), its bytes
 *     posted in the stream's chunk size, and a verdict of OS64_FETCH_OK,
 *     always: the record does not say how the fetch ended, only what the
 *     parser was fed; a file's page is read from the kept bytes by the
 *     window's own open_local;
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
 *     <url>` (rp_answer); and a kept line no job took is a fetch the page
 *     made where it was kept and not here, `REPLAY incomplete never asked
 *     for <kind> <url>` (rp_left_over). Pictures and fonts are never
 *     answered: they are not kept, and a page shows without them, as it
 *     does while they come. SO A REPLAY DOES NOT REPRODUCE WHAT THEIR
 *     FETCHES DECIDE: its record names those kinds on a `REPLAY not
 *     reproduced:` line, and its counts leave them out (RP_NOT_REPRODUCED);
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
 *     or nothing at all when the budget was spent), that asked for the
 *     next turn for ever, or that had a job out when the budget ended
 *     gains `REPLAY unfinished <why>`, and yonder_corpus_diff.py calls it
 *     broken, never progress;
 *   - the page left as yonder leaves it when it closes, which is the
 *     departure write: that record, its state second and its REPLAY lines
 *     after it, goes to OUT-DIR under the same name (rp_compose), and one
 *     line is printed with what the page came to: `unfinished` or
 *     `incomplete` with the first REPLAY line of that state, `clean` or
 *     the counts of the kinds a replay reproduces, or `unjudged` and why,
 *     and whether it did not let go of memory. A summary closes:
 *     `replayed P, clean C, incomplete I, unfinished U`, and how many
 *     records are not kept pages.
 *
 * UNJUDGED, which makes the answer 2 and writes a record of the page that
 * says so and nothing else: a `*.txt` that is not a whole record
 * (rp_record_read) or cannot be read, a record that says it lost lines
 * (whatever the cause: the line it lost may be the one a change made) or
 * was never written or was `kept: not whole`, kept bytes that are gone or
 * not as many as its `bytes:`, an index not as long as its `kept lines:`, a
 * kept file that is a symbolic link, a
 * folder whose index is missing, unreadable, of another format, damaged,
 * or names a file that is not there, a harness check that failed while
 * the page ran, a window record that is not whole or lost lines, and a
 * record that cannot be written whole.
 *
 * --self-test is the proof (run by tools/test_yonder_replay_host.sh): the
 * fixture pages the scripted-page host's fixtures serve
 * (tools/yonder_loop_fixture, tools/yonder_script_fixture, tools/pages) and
 * hand-made ones are loaded with keep_pages on, as a person browsing would,
 * their sub-resources served by the runner; what yonder kept is written out
 * to WORK-DIR/kept beside the records it wrote, replayed into WORK-DIR/out
 * by the path above, and each replayed record must be the page's own
 * (rp_same: both were read in this harness, which answers no picture or
 * font either way). Then copies of one, and pages written here, are
 * replayed as corpora that must not pass silently (rp_corpus_cases), the
 * attacks on both invariants among them.
 */
#define YONDER_HOST_FAKES_ONLY
#pragma GCC diagnostic ignored "-Wunused-function"
#include "test_yonder_scripts_host.c"

#include <ctype.h>
#include <limits.h>
#include <unistd.h>
#include "test_yonder_replay_fs.h"

/* THE DOOR HELD BY THE COMPILER: from here on, every host file function
 * this unit could reach is poisoned, so a file this runner touches goes
 * through test_yonder_replay_fs.h, whose faults are answers. That is the
 * list below and no more: a function it does not name, or one called
 * through a pointer, or code compiled before this line (the fakes and the
 * window above, whose raw file readers are fenced out of a fakes-only
 * build), is not held by it. */
#pragma GCC poison fopen fopen64 freopen freopen64 fdopen open open64 openat openat64 creat creat64 \
    opendir fdopendir readdir readdir64 scandir scandir64 stat stat64 fstat fstat64 lstat lstat64 \
    fstatat fstatat64 statx access faccessat fread fwrite fgets fgetc getc fputs fputc putc fscanf \
    fclose fseek ftell rewind fileno rename renameat renameat2 unlink unlinkat remove \
    mkdir mkdirat rmdir chmod fchmod chown truncate ftruncate realpath readlink readlinkat link linkat \
    symlink symlinkat mknod mkfifo tmpfile tmpfile64 tmpnam mkstemp mkdtemp popen system syscall \
    dup dup2 pipe sendfile mmap

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
 * gives back the data's bytes exactly. A new string, and its length in
 * `*len` when asked, since an escaped NUL may be among them. */
static char *rp_unescape_n(const char *s, size_t n, bool tokens, size_t *len) {
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
    if(len!=NULL) *len=o;
    return out;
}
static char *rp_unescape(const char *s, size_t n, bool tokens) { return rp_unescape_n(s,n,tokens,NULL); }

/* ── What the record says ───────────────────────────────────────────── */

/* WHAT A PAGE CAME TO, in order of precedence (a page that is two is the
 * later), as tools/yonder_records.py's State orders it. The runner decides
 * it from its own run (rp_decide) and writes it as the second line of the
 * page's record, `REPLAY state: <state> <why>`. */
typedef enum { RP_VERDICT, RP_INCOMPLETE, RP_UNFINISHED, RP_UNJUDGED, RP_STATES } RpState;
static const char *const kStates[RP_STATES]={"verdict","incomplete","unfinished","unjudged"};

/* WHAT A REPLAY DOES NOT REPRODUCE: the outcome of a fetch it never
 * performs. A page's pictures and fonts are never answered (rp_answers),
 * so a line whose outcome needed one of their fetches is not the page's on
 * a replay's record: yonder writes such a line under its own kind, a
 * MISSING `image` (a fetched picture's format) or a FAILED `picture-fetch`
 * or `font-fetch` (what a fetch, or the bytes it brought, came to). Every
 * replayed record names them on a `REPLAY not reproduced:` line, its
 * counts leave them out, and yonder_corpus_diff.py compares neither side's
 * lines of them. A picture or a font decided without a fetch (a scheme
 * nothing fetches, an address refused, a family no face names) is
 * reproduced and compared. */
#define RP_NOT_REPRODUCED "font-fetch image picture-fetch"
static bool rp_reproduced(const char *kind, size_t len) {
    for(const char *w=RP_NOT_REPRODUCED; *w; ) {
        size_t n=strcspn(w," ");
        if(n==len && !memcmp(w,kind,len)) return false;
        w+=n+(w[n]==' ');
    }
    return true;
}

enum { F_STATUS, F_FINAL, F_BODY, F_TYPE, F_CHARSET, F_ENCRYPTED, F_TLS, F_SCRIPTS, F_LAID, F_KEPT, F_BYTES,
       F_KEPT_LINES, F_N };
static const char *const kFacts[F_N]={"status","final address","body","content-type","charset","encrypted",
    "TLS","scripts","laid out at","kept","bytes","kept lines"};
/* A page file as yonder writes it (diag.c): a kept page's, or the one the
 * window writes in a replay. */
typedef struct {
    char *address;          /* line one, decoded */
    char *facts[F_N];       /* by kFacts, decoded; NULL when absent */
    unsigned missing, failed;   /* its record lines */
    unsigned replayed_missing, replayed_failed; /* those of the kinds a replay reproduces */
    const char *unjudged;   /* why the record says it cannot be judged; NULL when it does not */
} RpRecord;

static void rp_record_free(RpRecord *r) {
    free(r->address);
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
/* Whether `n` bytes are UTF-8: what tools/yonder_records.py decodes them
 * as, and refuses when they are not. */
static bool rp_utf8(const unsigned char *s, size_t n) {
    for(size_t i=0;i<n;) {
        unsigned c=s[i]; size_t more; unsigned min;
        if(c<0x80) { i++; continue; }
        if(c>=0xC2 && c<=0xDF) { more=1; min=0x80; }
        else if(c>=0xE0 && c<=0xEF) { more=2; min=0x800; }
        else if(c>=0xF0 && c<=0xF4) { more=3; min=0x10000; }
        else return false;
        unsigned v=c&(0x3F>>more);
        for(size_t k=1;k<=more;k++) {
            if(i+k>=n || (s[i+k]&0xC0)!=0x80) return false;
            v=(v<<6)|(s[i+k]&0x3F);
        }
        if(v<min || v>0x10FFFF || (v>=0xD800 && v<=0xDFFF)) return false;
        i+=more+1;
    }
    return true;
}
/* Decimal digits at `*s`, at least one, read on and saturating; false when
 * there are none. */
static bool rp_digits(const char **s, unsigned *v) {
    const char *p=*s; unsigned n=0;
    if(!isdigit((unsigned char)*p)) return false;
    for(; isdigit((unsigned char)*p); p++) n=n>(UINT_MAX-9)/10 ? UINT_MAX : n*10+(unsigned)(*p-'0');
    *s=p; *v=n;
    return true;
}
/* A verdict line in the one grammar tools/yonder_records.py reads:
 * `verdict: clean`, or the counts of the tokens with records,
 * `verdict: N MISSING, M FAILED`, `verdict: N MISSING`, `verdict: M
 * FAILED`, and nothing else on the line. */
static bool rp_verdict(const char *v, unsigned *missing, unsigned *failed) {
    *missing=*failed=0;
    if(!strcmp(v,"verdict: clean")) return true;
    if(strncmp(v,"verdict: ",9)) return false;
    const char *p=v+9; unsigned a=0, b=0;
    if(!rp_digits(&p,&a)) return false;
    if(!strcmp(p," FAILED")) { *failed=a; return true; }
    if(!strcmp(p," MISSING")) { *missing=a; return true; }
    if(strncmp(p," MISSING, ",10)) return false;
    p+=10;
    if(!rp_digits(&p,&b) || strcmp(p," FAILED")) return false;
    *missing=a; *failed=b;
    return true;
}
/* A KEPT PAGE'S RECORD READ WHOLE, by the grammar tools/yonder_records.py
 * reads it by: NULL with `r` filled, or why it is not a whole record (`r`
 * empty). Its first line `address: `, its second a verdict (rp_verdict) or
 * YONDER_DIAG_UNWRITTEN, its record lines exactly the verdict's count,
 * each counted, no other verdict line, no carriage return and no NUL, and
 * no `REPLAY` line but YONDER_DIAG_LINES_LOST: a record that says its name
 * was never written, or that it lost lines, is read, and says why it
 * cannot be judged (`unjudged`). */
static const char *rp_record_read(const char *text, size_t size, RpRecord *r) {
    memset(r,0,sizeof(*r));
    if(strncmp(text,"address: ",9)!=0) return "it is not a page file: its first line is not `address: `";
    if(memchr(text,'\0',size)!=NULL) return "it is not a whole page file: it holds a NUL";
    if(!rp_utf8((const unsigned char *)text,size)) return "it is not a page file: it is not UTF-8";
    if(strchr(text,'\r')!=NULL) return "it is not a whole page file: a line ends in a carriage return";
    unsigned want_missing=0, want_failed=0, line_no=0;
    const char *why=NULL;
    for(const char *line=text; *line && why==NULL; line_no++) {
        const char *end=strchr(line,'\n'); size_t len=end ? (size_t)(end-line) : strlen(line);
        char *v=strndup(line,len);
        /* The fields tools/yonder_records.py decodes, the address and the
         * record lines', must escape to UTF-8 as they must there. */
        if(line_no==0 || !strncmp(line,"MISSING ",8) || !strncmp(line,"FAILED ",7)) {
            size_t flen=0;
            char *field=rp_unescape_n(line,len,true,&flen);
            if(!rp_utf8((const unsigned char *)field,flen)) why="it is not a page file: a field escapes to bytes that are not UTF-8";
            free(field);
        }
        if(why!=NULL) { free(v); break; }
        if(line_no==0) r->address=rp_unescape(line+9,len-9,true);
        else if(line_no==1 && !strcmp(v,YONDER_DIAG_UNWRITTEN))
            r->unjudged="its name was claimed and its record never written";
        else if(line_no==1) {
            if(!rp_verdict(v,&want_missing,&want_failed)) why="it is not a whole page file: its second line is not a verdict";
        } else if(!strncmp(line,"MISSING ",8) || !strncmp(line,"FAILED ",7)) {
            if(!rp_counted(line,len)) why="it is not a whole page file: a record line with no count";
            else {
                const char *kind=line+(line[0]=='M' ? 8 : 7);
                const char *stop=line[0]=='M' ? memchr(kind,' ',(size_t)(line+len-kind)) : strstr(kind,": ");
                bool counted=rp_reproduced(kind,stop!=NULL && stop<line+len ? (size_t)(stop-kind) : (size_t)(line+len-kind));
                if(line[0]=='M') { r->missing++; r->replayed_missing+=counted; }
                else { r->failed++; r->replayed_failed+=counted; }
            }
        } else if(!strncmp(line,"verdict: ",9)) {
            why="it is not a whole page file: a second verdict line";
        } else if(!strcmp(v,YONDER_DIAG_LINES_LOST)) {
            r->unjudged="its record lost lines it could not keep";
        } else if(!strncmp(line,"REPLAY ",7)) {
            why="it is not a kept page's record: it carries a REPLAY line";
        } else {
            for(int i=0;i<F_N;i++) {
                size_t k=strlen(kFacts[i]);
                if(r->facts[i]==NULL && len>k+1 && !strncmp(line,kFacts[i],k) && line[k]==':' && line[k+1]==' ')
                    r->facts[i]=rp_unescape(line+k+2,len-k-2,true);
            }
        }
        free(v);
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
typedef struct { char *folder; RpEntry *e; size_t n; unsigned lines; } RpIndex;
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
 * line whose number is not one (rp_number) or that says neither `came`
 * nor `failed`, or names a file that is not there or cannot be read, is a
 * kept page with a piece missing. */
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
        x->lines+=line[0]!='#' && line[0]!='\0';
        if(line[0]!='#' && !rp_number(f[0]))
            bad="its index is damaged: a line's number is not a decimal number";
        else if(line[0]!='#' && nf==6 && (!strcmp(f[1],"script") || !strcmp(f[1],"sheet")) &&
                strcmp(f[3],"came") && strcmp(f[3],"failed"))
            bad="its index is damaged: a line says neither `came` nor `failed`";
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

/* A growing text: the replay's own lines, and the record a page's replay
 * writes. */
typedef struct { char *p; size_t n; } RpText;
static void rp_add(RpText *t, const char *s, size_t n) {
    t->p=realloc(t->p,t->n+n+1); memcpy(t->p+t->n,s,n); t->n+=n; t->p[t->n]='\0';
}
/* `data` as a page file writes data (diag.c): a byte that would break the
 * line, and a backslash, escaped, a token with its second letter
 * percent-encoded, and a `%` a reader would take for part of a spelling as
 * `%25`. */
static void rp_add_escaped(RpText *t, const char *data) {
    for(const char *s=data; *s; s++) {
        unsigned char c=(unsigned char)*s; char e[8];
        if(!strncmp(s,"MISSING",7) || !strncmp(s,"FAILED",6)) { snprintf(e,sizeof(e),"%c%%%02X",s[0],(unsigned char)s[1]); s++; }
        else if(c=='%' && (!strncmp(s+1,"25",2) || (s>data && s[-1]=='M' && !strncmp(s+1,"49SSING",7)) ||
                           (s>data && s[-1]=='F' && !strncmp(s+1,"41ILED",6)))) snprintf(e,sizeof(e),"%%25");
        else if(c=='\\') snprintf(e,sizeof(e),"\\\\");
        else if(c<0x20 || c==0x7F) snprintf(e,sizeof(e),"\\x%02X",c);
        else { e[0]=(char)c; e[1]='\0'; }
        rp_add(t,e,strlen(e));
    }
}

/* ── The pool, run here ─────────────────────────────────────────────── */

/* Every job the window submits, in the order it did; `unknown` once its
 * source had nothing for it, which leaves it out for good. */
typedef struct { os64_work_id_t id; os64_work_t work; uint32_t kind; bool unknown; } RpJob;
static RpJob *rp_jobs; static size_t rp_njobs, rp_capjobs;
static os64_work_id_t rp_ids;
static rp_lookup_t rp_lookup; static void *rp_lookup_ctx;
/* THE REPLAY'S OWN LINES (`rp_said`), which the replayed record gains
 * after what the window wrote: `REPLAY incomplete <what>` for each thing
 * the page asked for that its source had nothing for, or its source kept
 * that it never asked for (`rp_incomplete` counts them), and `REPLAY
 * unfinished <why>` when the replay reached no verdict on the page
 * (`rp_unfinished`). What they say is detail: the page's state is decided
 * from them by rp_decide and written on the record's second line. Both
 * are neither token, so a grep for MISSING or FAILED never meets them. */
static RpText rp_said;
static unsigned rp_incomplete;
static bool rp_unfinished;
/* A page from the network starts with a navigation job of the load's own;
 * any other is a navigation the page started itself, and the replay stops
 * there (`rp_navigated`). */
static bool rp_trip_expected, rp_navigated;

/* One REPLAY line: `head`, then `data` escaped as a page file writes it. */
static void rp_line(const char *head, const char *data) {
    rp_add(&rp_said,"REPLAY ",7); rp_add(&rp_said,head,strlen(head));
    rp_add_escaped(&rp_said,data);
    rp_add(&rp_said,"\n",1);
}
/* The first line of each state as it was said, before it was escaped:
 * the state line says it again, and escapes it once (rp_compose). */
static char rp_first[RP_STATES][OS64_FETCH_URL_MAX+128];
static void rp_incomplete_add(const char *kind, const char *url) {
    char head[64]; snprintf(head,sizeof(head),"incomplete %s ",kind);
    rp_line(head,url);
    if(!rp_incomplete) snprintf(rp_first[RP_INCOMPLETE],sizeof(rp_first[0]),"%s %s",kind,url);
    rp_incomplete++;
}
static void rp_unfinished_add(const char *why) {
    rp_line("unfinished ",why);
    if(!rp_unfinished) snprintf(rp_first[RP_UNFINISHED],sizeof(rp_first[0]),"%s",why);
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
/* The replay is over, and what is left says so. A job still waiting for
 * its line's turn never got it (the line before it named a job this replay
 * never asked for), so it is incomplete, as one with no line is. A job
 * whose turn it was, still out when the budget ended, makes the page
 * UNFINISHED: the replay stopped before answering what the page asked for
 * (a page that went elsewhere has stopped already, and says so). And a
 * kept line no job took is a fetch the page made where it was kept and not
 * here, so the page is incomplete too, symmetric with a job no line
 * answers. */
static void rp_left_over(void) {
    for(size_t i=0;i<rp_njobs;i++) {
        RpJob *j=&rp_jobs[i];
        if(!rp_answers(j) || j->kind==YONDER_JOB_TRIP) continue;
        const char *kind=j->kind==YONDER_JOB_SCRIPT ? "script" : "sheet";
        j->unknown=true;
        if(!rp_turn_of(j)) rp_incomplete_add(kind,rp_job_url(j));
        else if(!rp_navigated) {
            char why[OS64_FETCH_URL_MAX+96];
            snprintf(why,sizeof(why),"its %s %s was still out when the budget ended",kind,rp_job_url(j));
            rp_unfinished_add(why);
        }
    }
    for(size_t i=0;rp_order!=NULL && i<rp_order->n;i++)
        if(rp_order->e[i].used==0) {
            char head[64];
            snprintf(head,sizeof(head),"never asked for %s",rp_order->e[i].kind==YONDER_JOB_SCRIPT ? "script" : "sheet");
            rp_incomplete_add(head,rp_order->e[i].url);
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

/* A replay the self-test makes fail one of the harness's own checks: the
 * page at this address (NULL for none). */
static const char *rp_fail_check_at;

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
    rp_said.n=0; rp_incomplete=0; rp_unfinished=false;
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
    if(rp_fail_check_at!=NULL && !strcmp(load->address,rp_fail_check_at))
        check(false,"replay self-test: a harness check made to fail, to prove its page is unjudged");
    rp_left_over();
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
 * REPLAY line of its state says), and, for a VERDICT, the window record's
 * lines of the kinds a replay reproduces. */
typedef struct {
    RpState state;
    char why[1100];
    unsigned missing, failed;
} RpOutcome;

/* THE ONE PLACE A PAGE'S STATE IS DECIDED, from the page's own run, never
 * from a record read back: UNJUDGED when `unjudged` names why (the page
 * could not be read or replayed, a harness check failed, the window's
 * record lost lines or is not whole, or its record could not be written);
 * else UNFINISHED when the replay reached no verdict, INCOMPLETE when the
 * page and its kept fetches did not meet one for one; else VERDICT, with
 * the window's record lines of the kinds a replay reproduces (`window`),
 * none of which is clean. */
static RpOutcome rp_decide(const char *unjudged, const RpRecord *window) {
    RpOutcome o; memset(&o,0,sizeof(o));
    if(unjudged!=NULL) {
        o.state=RP_UNJUDGED;
        snprintf(o.why,sizeof(o.why),"%s",unjudged);
        return o;
    }
    o.state=rp_unfinished ? RP_UNFINISHED : rp_incomplete ? RP_INCOMPLETE : RP_VERDICT;
    if(o.state!=RP_VERDICT) snprintf(o.why,sizeof(o.why),"%s",rp_first[o.state]);
    o.missing=window->replayed_missing; o.failed=window->replayed_failed;
    return o;
}

/* THE RECORD A REPLAY WRITES, the page's state IN IT: its address; `REPLAY
 * state: <state> <why>` as its second line, before any token line; for a
 * judged state the window's record from its verdict on, the replay's own
 * REPLAY lines, and the kinds it does not reproduce; and `end`, which both
 * readers require, so a record cut short is never read. An UNJUDGED page's
 * record is its address, its state and `end`. `address` is the decoded
 * address, used when the window wrote no record. */
static RpText rp_compose(const RpOutcome *o, const char *address, const char *window, size_t wlen) {
    RpText t={0};
    const char *nl=window!=NULL ? memchr(window,'\n',wlen) : NULL;
    if(nl!=NULL) rp_add(&t,window,(size_t)(nl-window)+1);
    else { rp_add(&t,"address: ",9); rp_add_escaped(&t,address!=NULL ? address : ""); rp_add(&t,"\n",1); }
    rp_add(&t,"REPLAY state: ",14); rp_add(&t,kStates[o->state],strlen(kStates[o->state]));
    if(o->why[0]) { rp_add(&t," ",1); rp_add_escaped(&t,o->why); }
    rp_add(&t,"\n",1);
    if(o->state!=RP_UNJUDGED && nl!=NULL) {
        static const char kNot[]="REPLAY not reproduced: " RP_NOT_REPRODUCED "\n";
        rp_add(&t,nl+1,wlen-(size_t)(nl+1-window));
        if(rp_said.n) rp_add(&t,rp_said.p,rp_said.n);
        rp_add(&t,kNot,sizeof(kNot)-1);
    }
    rp_add(&t,"end\n",4);
    return t;
}

/* ── Replaying a directory ──────────────────────────────────────────── */

/* What a directory's replay came to: how many pages came to each state,
 * how many of the judged were clean, and, beside them, the records that
 * are not kept pages (no `kept: whole`), which are passed over, and the
 * pages that did not let go of what they held. An UNJUDGED page makes the
 * run's answer 2, since a corpus with pages nobody judged reads as one
 * that passed. */
typedef struct { unsigned states[RP_STATES], clean, bare, leaked; } RpTally;

/* The record's sequence, from its file name `<anything>-<digits>.txt`. */
static uint32_t rp_seq(const char *name) {
    size_t n=strlen(name);
    if(n<6 || strcmp(name+n-4,".txt")) return 0;
    size_t at=n-4; while(at>0 && isdigit((unsigned char)name[at-1])) at--;
    return at<n-4 && at>0 && name[at-1]=='-' ? (uint32_t)strtoul(name+at,NULL,10) : 0;
}

/* What the page came to, counted and printed on its line: the state, and
 * why or its counts. */
static void rp_report(const char *name, const RpOutcome *o, bool leaked, RpTally *t) {
    t->states[o->state]++;
    switch(o->state) {
    case RP_UNJUDGED: printf("%s  unjudged: %s",name,o->why); break;
    case RP_UNFINISHED: printf("%s  unfinished (%s)%s",name,o->why,rp_incomplete ? ", incomplete" : ""); break;
    case RP_INCOMPLETE: printf("%s  incomplete (%s)",name,o->why); break;
    case RP_VERDICT:
        if(o->missing==0 && o->failed==0) { t->clean++; printf("%s  clean",name); }
        else if(o->missing!=0 && o->failed!=0) printf("%s  %u MISSING, %u FAILED",name,o->missing,o->failed);
        else printf("%s  %u %s",name,o->missing ? o->missing : o->failed,o->missing ? "MISSING" : "FAILED");
        break;
    case RP_STATES: break;
    }
    if(leaked) { printf(" (it did not let go of what it held)"); t->leaked++; }
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

/* One kept page replayed into `out`. Only a record that says `kept:
 * whole` is a kept page; one that does not is passed over (`bare`), except
 * under `replace` (a partial run), where a record of it already in `out`
 * is replaced by one saying it is unjudged, so no record of an earlier run
 * stands for it. Every page that is replayed or cannot be writes its
 * record, its state in it, whole or not at all; under `replace` the
 * earlier record is removed first. False when that removal faults, which
 * refuses the rest of the run. */
static bool rp_replay_one(const char *kept, const char *out, const char *name, uint64_t budget_ms, bool replace,
                          RpTally *t) {
    char *stem=strndup(name,strlen(name)-4);
    char *base=rp_cat(kept,"/",stem);
    free(stem);
    RpFault fault;
    RpRecord r, window; memset(&r,0,sizeof(r)); memset(&window,0,sizeof(window));
    RpIndex index; memset(&index,0,sizeof(index));
    char *text=NULL, *bytes=NULL;
    const char *unjudged=NULL, *record=NULL;
    char why[1100];
    size_t size=0, len=0, wlen=0;
    bool ran=false, leaked=false, refused=false;
    rp_said.n=0; rp_incomplete=0; rp_unfinished=false;
    char *path=rp_cat(base,".txt","");
    RpFs got=rp_fs_read(path,&text,&size,&fault);
    free(path);
    if(got!=RP_FS_OK) { unjudged=got==RP_FS_FAULT ? fault.why : "it is gone"; goto judge; }
    if((unjudged=rp_record_read(text,size,&r))!=NULL) goto judge;
    if((unjudged=r.unjudged)!=NULL) goto judge;
    if(r.facts[F_KEPT]==NULL) {
        /* Not a kept page: keep_pages was off when it was written. */
        bool there=false;
        if(replace) {
            char *was=rp_cat(out,"/",name);
            bool dir=false;
            there=rp_fs_kind(was,&dir,&fault)!=RP_FS_ABSENT;
            free(was);
        }
        if(!there) { t->bare++; goto done; }
        unjudged="it is not a kept page (its record does not say `kept: whole`)";
        goto judge;
    }
    if(strcmp(r.facts[F_KEPT],"whole")) {
        snprintf(why,sizeof(why),"it was kept %s",r.facts[F_KEPT]);
        unjudged=why; goto judge;
    }
    path=rp_cat(base,".html","");
    got=rp_fs_read(path,&bytes,&len,&fault);
    free(path);
    if(got!=RP_FS_OK) { unjudged=got==RP_FS_FAULT ? fault.why : "its record says it was kept whole, and its bytes are gone"; goto judge; }
    /* NOTHING ON THE HOST BINDS A RECORD TO ITS KEPT FILES but what the
     * record says of them: the bytes the parser was fed, and the lines its
     * index holds. Kept files that are not those are another page's. */
    if(r.facts[F_BYTES]==NULL || strtoull(r.facts[F_BYTES],NULL,10)!=len || !isdigit((unsigned char)r.facts[F_BYTES][0])) {
        snprintf(why,sizeof(why),"its record says the page had %s bytes, and its kept bytes are %zu",
                 r.facts[F_BYTES]!=NULL ? r.facts[F_BYTES] : "no count of",len);
        unjudged=why; goto judge;
    }
    path=rp_cat(base,".d","");
    unjudged=rp_index_read(path,&index,&fault);
    free(path);
    if(unjudged!=NULL) goto judge;
    unsigned want_lines=r.facts[F_KEPT_LINES]!=NULL ? (unsigned)strtoul(r.facts[F_KEPT_LINES],NULL,10) : 0;
    if(want_lines!=index.lines) {
        snprintf(why,sizeof(why),"its record says its index holds %u lines, and it holds %u",want_lines,index.lines);
        unjudged=why; goto judge;
    }
    RpLoad load={0};
    load.address=r.address; load.seq=rp_seq(name); load.bytes=(const uint8_t *)bytes; load.len=len;
    load.local=!strncmp(r.address,"file://",7);
    load.scripts=r.facts[F_SCRIPTS]==NULL || strcmp(r.facts[F_SCRIPTS],"off")!=0;
    load.width=r.facts[F_LAID]!=NULL ? atoi(r.facts[F_LAID]) : 0;
    load.dir="/tmp/replay"; load.budget_ms=budget_ms;
    rp_head_of(&r,&load.head);
    fake_reset(); snprintf(fake_dirs[nfake_dirs++],256,"/tmp/replay");
    size_t live_before=live;
    RpRan run=rp_run(&load,rp_index_lookup,&index,&index);
    ran=true;
    leaked=run.live_after!=live_before;
    char written[256];
    record=rp_written("/tmp/replay",r.address,load.seq,written,sizeof(written),&wlen);
    const char *bad=NULL;
    if(run.harness_failures!=0) {
        snprintf(why,sizeof(why),"%u of the harness's own checks failed while it ran",run.harness_failures);
        unjudged=why;
    } else if(record==NULL) {
        unjudged="the window wrote no record of it";
    } else if((bad=rp_record_read(record,wlen,&window))!=NULL) {
        snprintf(why,sizeof(why),"the window's own record: %s",bad);
        unjudged=why;
    } else if(window.unjudged!=NULL) {
        snprintf(why,sizeof(why),"the window's own record: %s",window.unjudged);
        unjudged=why;
    }
judge:;
    RpOutcome o=rp_decide(unjudged,&window);
    RpText whole=rp_compose(&o,r.address,o.state==RP_UNJUDGED ? NULL : record,wlen);
    path=rp_cat(out,"/",name);
    /* A partial run removes the page's earlier record first, so a record
     * that cannot be written now leaves none rather than the old one; one
     * that cannot be removed refuses the run. */
    if(replace && rp_fs_remove(path,&fault)==RP_FS_FAULT) {
        fprintf(stderr,"replay: %s; the run stops here\n",fault.why);
        free(path); free(whole.p);
        refused=true;
        if(ran) fake_reset();
        goto done;
    }
    if(rp_fs_write(path,whole.p,whole.n,&fault)!=RP_FS_OK && o.state!=RP_UNJUDGED) {
        /* The record could not be written whole: the page cannot be judged,
         * and that is written instead, if it can be. */
        snprintf(why,sizeof(why),"its record could not be written: %s",fault.why);
        o=rp_decide(why,&window);
        free(whole.p);
        whole=rp_compose(&o,r.address,NULL,0);
        (void)rp_fs_write(path,whole.p,whole.n,&fault);
    }
    free(path); free(whole.p);
    rp_report(name,&o,ran && leaked,t);
    if(ran) fake_reset();
done:
    free(text); free(bytes); free(base);
    rp_index_free(&index); rp_record_free(&r); rp_record_free(&window);
    return !refused;
}

/* A whole replay (no `only`) is written into an OUT-DIR that holds no
 * records yet, and refused otherwise: a record left from an earlier run,
 * for a page since removed or no longer replayable, would be read by the
 * diff as that page's. A partial one (`only`) may add to one, and says how
 * many records there were before it. Either is refused before anything is
 * written when OUT-DIR or KEPT-DIR cannot be listed, OUT-DIR made, or the
 * two are one directory, whose kept records the replay would write over. */
static int rp_replay_dir(const char *kept, const char *out, uint64_t budget_ms, const char *only, RpTally *t) {
    RpFault fault;
    char **held=NULL, **names=NULL; size_t before=0, n=0;
    RpFs got=rp_fs_list(out,".txt",&held,&before,&fault);
    rp_fs_names_free(held,before);
    if(got==RP_FS_FAULT) { fprintf(stderr,"replay: %s; nothing was replayed\n",fault.why); return 2; }
    bool same=false;
    if(got==RP_FS_OK && (got=rp_fs_same(kept,out,&same,&fault))==RP_FS_FAULT) {
        fprintf(stderr,"replay: %s; nothing was replayed\n",fault.why); return 2;
    }
    if(same) {
        fprintf(stderr,"replay: %s is the kept directory itself; a replay is written somewhere else\n",out);
        return 2;
    }
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
        if(only==NULL || strstr(names[i],only)!=NULL) {
            bool going=rp_replay_one(kept,out,names[i],budget_ms,only!=NULL,t);
            fflush(stdout);
            if(!going) { rp_fs_names_free(names,n); return 2; }
        }
    rp_fs_names_free(names,n);
    unsigned replayed=t->states[RP_VERDICT]+t->states[RP_INCOMPLETE]+t->states[RP_UNFINISHED];
    printf("replayed %u, clean %u, incomplete %u, unfinished %u",replayed,t->clean,t->states[RP_INCOMPLETE],
           t->states[RP_UNFINISHED]);
    if(t->bare) printf("; %u records are not kept pages",t->bare);
    if(t->states[RP_UNJUDGED]) printf("; %u unjudged",t->states[RP_UNJUDGED]);
    if(t->leaked) printf("; %u did not let go of what they held",t->leaked);
    if(only!=NULL)
        printf("; a partial run (--only %s): %s held %zu records before it, which stand except those it "
               "wrote again",only,out,before);
    printf("\n");
    return t->states[RP_UNJUDGED] ? 2 : t->leaked ? 1 : 0;
}

/* ── The self-test ──────────────────────────────────────────────────── */

/* The self-test's own files go through the same door, and a fault is said
 * and counted as a problem (`rp_faults`, which the self-test adds to its
 * own): NULL from rp_file when the file is not there or could not be
 * read. */
static unsigned rp_faults;
static char *rp_file(const char *path, size_t *len) {
    RpFault fault; char *bytes=NULL;
    RpFs got=rp_fs_read(path,&bytes,len,&fault);
    if(got==RP_FS_FAULT) { printf("  %s\n",fault.why); rp_faults++; }
    return got==RP_FS_OK ? bytes : NULL;
}
static bool rp_put(const char *path, const void *bytes, size_t len) {
    RpFault fault;
    if(rp_fs_write(path,bytes,len,&fault)==RP_FS_OK) return true;
    printf("  %s\n",fault.why); rp_faults++;
    return false;
}
static bool rp_put_dir(const char *path) {
    RpFault fault;
    if(rp_fs_mkdir(path,&fault)==RP_FS_OK) return true;
    printf("  %s\n",fault.why); rp_faults++;
    return false;
}
/* A kept page's record written here, saying what yonder's says of its kept
 * files: as many bytes as `page` and `lines` index lines. */
static bool rp_put_record(const char *path, const char *record, const char *page, unsigned lines) {
    size_t n=strlen(record)+64;
    char *text=malloc(n);
    snprintf(text,n,"%sbytes: %zu\nkept lines: %u\n",record,strlen(page),lines);
    bool ok=rp_put(path,text,strlen(text));
    free(text);
    return ok;
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

/* A page the self-test kept must replay to its own record, each difference
 * said. Both were read in this harness, which answers no picture or font
 * either way, so here the record matches in its picture and font lines
 * too, which a corpus kept on the glass does not. Let be: a `paint stages`
 * line, by its key, whose times are the processor's counter
 * (YONDER_DIAGNOSTICS.md § What paints cost), which no clock of the
 * harness moves; the `kept:` and `kept lines:` facts, which say how the
 * record was made and not what the page did; and the replay's own lines, its state, what it
 * does not reproduce and its `end`. */
static bool rp_let_be(const char *line, size_t len, bool replay) {
    return (len>=6 && !strncmp(line,"kept: ",6)) || (len>=12 && !strncmp(line,"kept lines: ",12)) ||
           (replay && ((len>=14 && !strncmp(line,"REPLAY state: ",14)) ||
                       (len>=23 && !strncmp(line,"REPLAY not reproduced: ",23)) || (len==3 && !strncmp(line,"end",3))));
}
static bool rp_same(const char *name, const char *a, const char *b) {
    bool same=true;
    const char *x=a, *y=b;
    while(*x || *y) {
        const char *xe=strchr(x,'\n'), *ye=strchr(y,'\n');
        size_t xl=xe ? (size_t)(xe-x) : strlen(x), yl=ye ? (size_t)(ye-y) : strlen(y);
        if(*x && rp_let_be(x,xl,false)) { x+=xl+(xe?1:0); continue; }
        if(*y && rp_let_be(y,yl,true)) { y+=yl+(ye?1:0); continue; }
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
typedef enum { RP_COPY, RP_DROP_SCRIPT_LINE, RP_BAD_NUMBER, RP_NO_MEMORY, RP_FULL_TABLE, RP_NO_INDEX } RpEdit;

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
        if(edit==RP_NO_INDEX && !strcmp(kFiles[f],".d/index")) continue;
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
            /* A line taken out of the index is taken out of the record's
             * count too: the page kept one fetch fewer, not a damaged
             * index. */
            char *count=strstr(text,"\nkept lines: ");
            if(!strcmp(kFiles[f],".txt") && edit==RP_DROP_SCRIPT_LINE && count!=NULL) {
                unsigned lines=(unsigned)strtoul(count+13,NULL,10);
                char *end=strchr(count+1,'\n'), rest[64]="";
                snprintf(rest,sizeof(rest),"\nkept lines: %u",lines-1);
                size_t tail=strlen(end);
                char *again=malloc((size_t)(count-text)+strlen(rest)+tail+1);
                memcpy(again,text,(size_t)(count-text)); strcpy(again+(count-text),rest); strcat(again,end);
                free(text); text=again; len=strlen(text);
            }
            if(!strcmp(kFiles[f],".txt") && (edit==RP_NO_MEMORY || edit==RP_FULL_TABLE)) {
                const char *add=edit==RP_NO_MEMORY ? "written: no memory to write this line\n" YONDER_DIAG_LINES_LOST "\n"
                                                   : "facts not kept: 3\n" YONDER_DIAG_LINES_LOST "\n";
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

    /* An index line whose number is not one, a record that lost a line
     * (with no memory to write it, or no room: the line past the cap may be
     * the one a change made), and a `*.txt` that is not a record: none is
     * judged, each says so on a record of its own, and each makes the
     * answer 2. */
    static const struct { const char *dir; RpEdit edit; } kRefused[]={
        {"/badnumber",RP_BAD_NUMBER},{"/nomemory",RP_NO_MEMORY},{"/fulltable",RP_FULL_TABLE},{"/notarecord",RP_COPY}};
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
        char *said=rp_cat(dir,"-out/",kRefused[i].edit==RP_COPY ? "notes.txt" : name);
        char *unjudged=rp_file(said,NULL);
        if(rc!=2 || t.states[RP_UNJUDGED]!=1 || rp_replayed(&t)!=judged || unjudged==NULL ||
           strstr(unjudged,"\nREPLAY state: unjudged ")==NULL) {
            printf("  %s: a corpus with a page nobody can judge answered %d\n",kRefused[i].dir,rc); bad++;
        }
        free(unjudged); free(said); free(record); free(dir);
    }

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
        "final address: http://fixture.test/twice\nbody: html\ncontent-type: text/html\nencrypted: no\nkept: whole\n";
    static const char kTwicePage[]="<!doctype html><p>twice</p><script src=t.js></script><script src=t.js></script>";
    static const char kTwiceIndex[]="# yonder kept index 1\n1\tscript\thttp://fixture.test/t.js\tcame\thttp://fixture.test/t.js\t\n";
    char *f1=rp_cat(dir,"/fixture.test-0001.txt",""), *f2=rp_cat(dir,"/fixture.test-0001.html","");
    char *f3=rp_cat(dfolder,"/index",""), *f4=rp_cat(dfolder,"/1.js","");
    if(!rp_put_record(f1,kTwiceRecord,kTwicePage,1) || !rp_put(f2,kTwicePage,sizeof(kTwicePage)-1) ||
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
        "final address: http://fixture.test/order\nbody: html\ncontent-type: text/html\nencrypted: no\nkept: whole\n";
    static const char kOrderPage[]="<!doctype html><script>var order=[];</script>"
        "<script async src=a.js></script><script async src=b.js></script>"
        "<script>setTimeout(function(){if(order.join()!='b,a')wrongOrder()},1000)</script>";
    static const char kOrderIndex[]="# yonder kept index 1\n"
        "1\tscript\thttp://fixture.test/b.js\tcame\thttp://fixture.test/b.js\t\n"
        "2\tscript\thttp://fixture.test/a.js\tcame\thttp://fixture.test/a.js\t\n";
    f1=rp_cat(dir,"/fixture.test-0001.txt",""); f2=rp_cat(dir,"/fixture.test-0001.html","");
    f3=rp_cat(dfolder,"/index",""); f4=rp_cat(dfolder,"/1.js","");
    char *f5=rp_cat(dfolder,"/2.js","");
    if(!rp_put_record(f1,kOrderRecord,kOrderPage,2) || !rp_put(f2,kOrderPage,sizeof(kOrderPage)-1) ||
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
        "final address: http://fixture.test/goes\nbody: html\ncontent-type: text/html\nencrypted: no\nkept: whole\n";
    static const char kPage[]="<!doctype html><p>going</p><script>"
        "setTimeout(function(){location.href='http://fixture.test/next'},100);"
        "setTimeout(function(){tooLate()},5000)</script>";
    char *txt=rp_cat(dir,"/fixture.test-0001.txt",""), *html=rp_cat(dir,"/fixture.test-0001.html","");
    if(!rp_put_record(txt,kRecord,kPage,0) || !rp_put(html,kPage,sizeof(kPage)-1)) bad++;
    rc=rp_replay_copy(dir,"fixture.test-0001.txt",60000,&t,&record);
    if(rc!=0 || t.states[RP_INCOMPLETE]!=1 || t.clean!=0 || record==NULL ||
       strstr(record,"\nREPLAY incomplete navigation http://fixture.test/next\n")==NULL ||
       strstr(record,"tooLate")!=NULL) {
        printf("  a page that navigated was not stopped there\n"); bad++;
    }
    free(record); free(txt); free(html); free(dir);

    /* A folder whose index is not there is a kept page with a piece
     * missing, not a page that fetched nothing: unjudged. */
    dir=rp_cat(work,"/noindex","");
    bad+=rp_copy_page(kept,name,dir,RP_NO_INDEX);
    rc=rp_replay_copy(dir,name,60000,&t,&record);
    if(rc!=2 || t.states[RP_UNJUDGED]!=1 || rp_replayed(&t)!=0 || record==NULL ||
       strstr(record,"\nREPLAY state: unjudged its folder has no index\nend\n")==NULL) {
        printf("  a folder with no index was not unjudged (%d)\n",rc); bad++;
    }
    free(record);

    /* An OUT-DIR that can be searched and not listed: whether it holds
     * records cannot be told, so the whole replay is refused and nothing
     * is written. Root lists any directory, and skips it. */
    if(geteuid()!=0) {
        char *locked=rp_cat(work,"/unlisted","");
        RpFault fault;
        if(!rp_put_dir(locked) || rp_fs_mode(locked,0333,&fault)!=RP_FS_OK) bad++;
        RpTally t3={0};
        rc=rp_replay_dir(dir,locked,60000,NULL,&t3);
        if(rp_fs_mode(locked,0755,&fault)!=RP_FS_OK) bad++;
        char **held=NULL; size_t nheld=0;
        if(rc!=2 || rp_replayed(&t3)!=0 || rp_fs_list(locked,".txt",&held,&nheld,&fault)!=RP_FS_OK || nheld!=0) {
            printf("  a whole replay into an OUT-DIR that cannot be listed was not refused (%d)\n",rc); bad++;
        }
        rp_fs_names_free(held,nheld);
        free(locked);
    }
    free(dir);

    /* A page kept with a picture whose fetch failed: the replay fetches no
     * picture, so the record it writes has no line for it, and says it does
     * not reproduce a fetch's outcome, which the corpus diff then compares
     * on neither side; the page reads clean. */
    dir=rp_cat(work,"/picture","");
    if(!rp_put_dir(dir)) bad++;
    static const char kPicRecord[]="address: http://fixture.test/pic\nverdict: 1 FAILED\n"
        "FAILED picture-fetch: http://fixture.test/p.png: HTTP 404 (1)\nstatus: 200 OK\n"
        "final address: http://fixture.test/pic\nbody: html\ncontent-type: text/html\nencrypted: no\nkept: whole\n";
    static const char kPicPage[]="<!doctype html><p>a picture</p><img src=p.png>";
    char *pt=rp_cat(dir,"/fixture.test-0001.txt",""), *ph=rp_cat(dir,"/fixture.test-0001.html","");
    if(!rp_put_record(pt,kPicRecord,kPicPage,0) || !rp_put(ph,kPicPage,sizeof(kPicPage)-1)) bad++;
    rc=rp_replay_copy(dir,"fixture.test-0001.txt",60000,&t,&record);
    if(rc!=0 || t.clean!=1 || record==NULL || strstr(record,"FAILED")!=NULL ||
       strstr(record,"\nREPLAY not reproduced: " RP_NOT_REPRODUCED "\n")==NULL) {
        printf("  a page kept with a failed picture did not replay clean, saying pictures are not reproduced\n"); bad++;
    }
    free(record); free(pt); free(ph); free(dir);

    /* A harness check that fails while a page replays: the page is
     * UNJUDGED on its own record, never a verdict the diff would judge.
     * The check made to fail is taken back once its page is seen. */
    dir=rp_cat(work,"/checkfail","");
    bad+=rp_copy_page(kept,name,dir,RP_COPY);
    unsigned failed_before=failures;
    rp_fail_check_at="http://fixture.test/kept";
    rc=rp_replay_copy(dir,name,60000,&t,&record);
    rp_fail_check_at=NULL;
    if(failures!=failed_before+1) bad++;
    failures=failed_before;
    if(rc!=2 || t.states[RP_UNJUDGED]!=1 || record==NULL ||
       strstr(record,"\nREPLAY state: unjudged 1 of the harness's own checks failed while it ran\nend\n")==NULL) {
        printf("  a page whose harness check failed was not unjudged on its record (%d)\n",rc); bad++;
    }
    free(record);

    /* A partial replay into the kept directory itself is refused: it would
     * write over the kept records. */
    memset(&t,0,sizeof(t));
    if(rp_replay_dir(dir,dir,60000,"fixture",&t)!=2 || rp_replayed(&t)!=0) {
        printf("  a partial replay into the kept directory was not refused\n"); bad++;
    }
    free(dir);

    /* A kept script the page no longer asks for: the page and its kept
     * fetches did not meet one for one, so it is INCOMPLETE, never a page
     * that stopped running a script. Its address holds a `%25` and a
     * backslash, which the state line says escaped once, as the detail
     * line does. */
    dir=rp_cat(work,"/unasked","");
    dfolder=rp_cat(dir,"/fixture.test-0001.d","");
    if(!rp_put_dir(dir) || !rp_put_dir(dfolder)) bad++;
    static const char kUnaskedRecord[]="address: http://fixture.test/unasked\nverdict: clean\nstatus: 200 OK\n"
        "final address: http://fixture.test/unasked\nbody: html\ncontent-type: text/html\nencrypted: no\nkept: whole\n";
    static const char kUnaskedPage[]="<!doctype html><p>no script now</p>";
    static const char kUnaskedIndex[]="# yonder kept index 1\n"
        "1\tscript\thttp://fixture.test/a%25b\\\\c.js\tcame\thttp://fixture.test/a%25b\\\\c.js\t\n";
    f1=rp_cat(dir,"/fixture.test-0001.txt",""); f2=rp_cat(dir,"/fixture.test-0001.html","");
    f3=rp_cat(dfolder,"/index",""); f4=rp_cat(dfolder,"/1.js","");
    if(!rp_put_record(f1,kUnaskedRecord,kUnaskedPage,1) || !rp_put(f2,kUnaskedPage,sizeof(kUnaskedPage)-1) ||
       !rp_put(f3,kUnaskedIndex,sizeof(kUnaskedIndex)-1) || !rp_put(f4,"nothing()",9)) bad++;
    rc=rp_replay_copy(dir,"fixture.test-0001.txt",60000,&t,&record);
    if(rc!=0 || t.states[RP_INCOMPLETE]!=1 || t.clean!=0 || record==NULL ||
       strstr(record,"\nREPLAY state: incomplete never asked for script http://fixture.test/a%2525b\\\\c.js\n")==NULL ||
       strstr(record,"\nREPLAY incomplete never asked for script http://fixture.test/a%2525b\\\\c.js\n")==NULL) {
        printf("  a kept script the page never asked for did not read as incomplete\n"); bad++;
    }
    free(record); free(f1); free(f2); free(f3); free(f4); free(dfolder); free(dir);

    /* A record yonder wrote with keep_pages off, beside page bytes of the
     * same name: not a kept page, passed over and never replayed. Under a
     * partial run, a record of it already written is replaced by one that
     * says it is unjudged, so no earlier run's record stands for it. */
    dir=rp_cat(work,"/notkept","");
    char *stale=rp_cat(dir,"-out","");
    if(!rp_put_dir(dir) || !rp_put_dir(stale)) bad++;
    static const char kNotKept[]="address: http://fixture.test/plain\nverdict: clean\nstatus: 200 OK\n"
        "final address: http://fixture.test/plain\nbody: html\ncontent-type: text/html\nencrypted: no\n";
    static const char kStale[]="address: http://fixture.test/plain\nREPLAY state: verdict\nverdict: clean\nend\n";
    f1=rp_cat(dir,"/fixture.test-0001.txt",""); f2=rp_cat(dir,"/fixture.test-0001.html","");
    if(!rp_put(f1,kNotKept,sizeof(kNotKept)-1) || !rp_put(f2,"<p>another's bytes</p>",22)) bad++;
    memset(&t,0,sizeof(t));
    char *out=rp_cat(dir,"-fresh","");
    if(rp_replay_dir(dir,out,60000,NULL,&t)!=0 || rp_replayed(&t)!=0 || t.bare!=1) {
        printf("  a record not kept whole beside page bytes was replayed\n"); bad++;
    }
    free(out);
    f3=rp_cat(stale,"/fixture.test-0001.txt","");
    if(!rp_put(f3,kStale,sizeof(kStale)-1)) bad++;
    memset(&t,0,sizeof(t));
    rc=rp_replay_dir(dir,stale,60000,"fixture",&t);
    record=rp_file(f3,NULL);
    if(rc!=2 || t.states[RP_UNJUDGED]!=1 || record==NULL || strstr(record,"\nREPLAY state: unjudged ")==NULL) {
        printf("  a partial run left an earlier record standing for a page no longer kept\n"); bad++;
    }
    free(record); free(f1); free(f2); free(f3); free(stale); free(dir);

    /* Kept bytes that are not the record's: its `bytes:` says otherwise,
     * and nothing else on the host would tell. */
    dir=rp_cat(work,"/otherbytes","");
    bad+=rp_copy_page(kept,name,dir,RP_COPY);
    char *stem=strndup(name,strlen(name)-4);
    f1=rp_cat(dir,"/",stem); f2=rp_cat(f1,".html","");
    if(!rp_put(f2,"<p>another page's bytes</p>",27)) bad++;
    rc=rp_replay_copy(dir,name,60000,&t,&record);
    if(rc!=2 || t.states[RP_UNJUDGED]!=1 || record==NULL || strstr(record,"its kept bytes are 27\n")==NULL) {
        printf("  kept bytes of the wrong length were replayed\n"); bad++;
    }
    free(record); free(f2); free(f1); free(stem);

    /* A partial run whose new record cannot be written (its temporary's
     * name is taken by a directory) leaves no record, never the old one;
     * one whose old record cannot be removed (a directory with something
     * in it, by that name) refuses the run. */
    char *was=rp_cat(dir,"-out/",name);
    if(rp_put(was,"address: stale\n",15)) {
        char temp[4096]; snprintf(temp,sizeof(temp),"%s.%ld.new",was,(long)getpid());
        if(!rp_put_dir(temp)) bad++;
        memset(&t,0,sizeof(t));
        char *outdir=rp_cat(dir,"-out","");
        rc=rp_replay_dir(dir,outdir,60000,"fixture",&t);
        record=rp_file(was,NULL);
        RpFault fault;
        if(rc!=2 || record!=NULL) {
            printf("  a partial run whose record could not be written left the old one standing\n"); bad++;
        }
        free(record);
        if(rp_fs_remove(temp,&fault)!=RP_FS_OK) bad++;
        char inside[4096]; snprintf(inside,sizeof(inside),"%s/x",was);
        if(!rp_put_dir(was) || !rp_put(inside,"x",1)) bad++;
        memset(&t,0,sizeof(t));
        if(rp_replay_dir(dir,outdir,60000,"fixture",&t)!=2 || rp_replayed(&t)!=0) {
            printf("  a partial run that could not remove an old record went on\n"); bad++;
        }
        free(outdir);
    } else bad++;
    free(was); free(dir);

    /* A `*.txt` named only `.txt`, and a record that is not UTF-8: each is
     * read, and unjudged, never passed over. */
    dir=rp_cat(work,"/oddnames","");
    if(!rp_put_dir(dir)) bad++;
    f1=rp_cat(dir,"/.txt",""); f2=rp_cat(dir,"/latin-0002.txt","");
    static const char kLatin[]="address: http://fixture.test/caf\xE9\nverdict: clean\n";
    if(!rp_put(f1,"not a page file\n",16) || !rp_put(f2,kLatin,sizeof(kLatin)-1)) bad++;
    memset(&t,0,sizeof(t));
    char *outdir=rp_cat(dir,"-out","");
    if(rp_replay_dir(dir,outdir,60000,NULL,&t)!=2 || t.states[RP_UNJUDGED]!=2) {
        printf("  a file named `.txt`, or a record that is not UTF-8, was passed over\n"); bad++;
    }
    free(outdir); free(f1); free(f2); free(dir);
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
    bad+=rp_faults;
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
