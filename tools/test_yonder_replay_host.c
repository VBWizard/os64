/* THE REPLAY RUNNER (MODERN_WEB.md rule 3): a page yonder kept beside its
 * page file (yonder.conf's keep_pages, diag.h) fed back to yonder's own
 * window on the host, with no network, and its record written again.
 * docs/design/pending/REPLAY.md is the contract this file is held to: the
 * files of a kept page (§ 1), the grammar of a record and an index (§ 2),
 * the replay's inputs and the versions of a kept page (§ 3), the index
 * (§ 4), what a replay reproduces and the page's state (§ 5), one process
 * per page (§ 6), and the proof (§ 7). What follows is how this file
 * meets it.
 *
 *     test_yonder_replay_host KEPT-DIR OUT-DIR [--seconds N] [--only TEXT] [--window KEY=VALUE,...]
 *                             [--crash-page NAME]
 *     test_yonder_replay_host --self-test WORK-DIR
 *
 * A whole replay is written into an OUT-DIR with no records in it, and is
 * refused otherwise, since a record left from an earlier run would read as
 * that page's; `--only` replays the records whose names hold TEXT and may
 * add to one, saying how many it found there, and a page it finds no
 * longer kept replaces its earlier record with one saying it is unjudged.
 * An OUT-DIR or KEPT-DIR that cannot be listed, or an OUT-DIR that is the
 * KEPT-DIR, refuses the run before anything is written. `--window
 * KEY=VALUE,...` states what a version 1 corpus's window was when it was
 * recorded (REPLAY.md § 3; rp_window_state), and states nothing for a
 * version 2 page. Every file this
 * runner touches goes through test_yonder_replay_fs.h, whose writes are
 * whole or not at all and whose faults become an UNJUDGED page or a
 * refused run, never an empty answer, and the host's file functions are
 * poisoned below it.
 *
 * THE READERS: rp_classify is § 2's grammar, line by line, and runs every
 * line's vector in tools/yonder_record_grammar.txt (rp_grammar_vectors), as
 * tools/yonder_records.py's classify() does; rp_record_read reads a kept
 * record whole by it, and the vectors of a whole record yonder wrote are
 * read through it, as through tools/yonder_records.py's read(); rp_index_read reads a kept
 * index, and every number goes through rp_decimal.
 *
 * ONE PROCESS PER PAGE (rp_one_process): the parent decides from the kept
 * record whether a page is one to replay, forks a child for it, and
 * trusts what the child says only when its exit status and its record
 * agree; the child (rp_replay_page) reads the page's inputs from its record
 * (rp_window_of: every one required of a version 2 page; a version 1
 * page's window, agent and zone, when its record does not say them, as
 * `--window` states them, and its cookies and clock the harness's, each
 * named on its record), sets them where the window reads them (rp_run,
 * rp_apply_inputs), feeds the page to the window, writes its record with
 * the state rp_decide gave it, and exits.
 *
 * THE REPLAY ITSELF (rp_run). The window and every fake are
 * tools/test_yonder_scripts_host.c's, the headless binding: this file
 * includes it for them and brings its own main.
 *
 *   - start_trip to the address on the record's first line. A page from the
 *     network gets a head rebuilt from the record's facts (status, final
 *     address, body, content-type, charset, encrypted, TLS), its bytes
 *     posted in the stream's chunk size, and a verdict of OS64_FETCH_OK,
 *     always: the record does not say how the fetch ended, only what the
 *     parser was fed; a file's page is read from the kept bytes by the
 *     window's own open_local;
 *   - the loop turned as yonder's main loop turns it when nobody touches
 *     the page (the stream's turn, the waiting page's, the shown page's,
 *     the pictures' settle, the tick that shows a page whose sheets are
 *     late; no input is dispatched, so what a click or a key did where the
 *     page was kept is not reproduced, REPLAY.md § 5), with the jobs the window
 *     submitted answered at the top of each turn: a script or a sheet from
 *     the kept index's line for it (rp_entry_is), each line answering one
 *     job, and IN THE INDEX'S ORDER, which is the order the products
 *     reached the window where the page was kept: a job whose line comes
 *     later waits for it, so async scripts run in the order they ran there
 *     (rp_order), and a job still waiting when the replay ends is
 *     incomplete. One the index has no line for, or no line left for
 *     (asked more often than where it was kept), is not made up either
 *     way: it stays out, as if still on the wire, and the record gains
 *     `REPLAY incomplete <kind> <url>` (rp_answer); and a kept line no job
 *     took is a fetch the page made where it was kept and not here,
 *     `REPLAY incomplete never asked for <kind> <url>` (rp_left_over).
 *     Pictures and fonts are never answered (RP_NOT_REPRODUCED);
 *   - THE CLOCK. A turn with nothing to do moves the window's milliseconds
 *     on by a tick (10 ms), until the page is quiet (nothing arriving,
 *     nothing out that will be answered, no timer set) or the virtual
 *     budget is spent (60 s, --seconds; the record says it, and the diff
 *     compares no two replays given different budgets), so a page's timers
 *     fire as they would on the glass. The microsecond clock also moves 1 ms each time
 *     it is read, so a script that never ends is stopped at the script
 *     time limit as it is on the P5. The JS Date clock is the record's
 *     `clock:` (a version 1 page's the harness's) and does not move;
 *   - A NAVIGATION THE PAGE STARTS (a script setting `location`, a
 *     refresh, a form it sends) ends the replay there: the next page is
 *     not in this one's index, and a page left running would fire timers
 *     that never fired where it was kept. The record gains `REPLAY
 *     incomplete navigation <url>`;
 *   - a page that never arrived (its parse unfinished, its sheets awaited,
 *     or nothing at all when the budget was spent), that asked for the
 *     next turn for ever, or that had a job out when the budget ended
 *     gains `REPLAY unfinished <why>`;
 *   - the page left as yonder leaves it when it closes and nobody touched
 *     it, which is the
 *     departure write: that record, its state second and its REPLAY lines
 *     after it, goes to OUT-DIR under the same name (rp_compose), and one
 *     line is printed with what the page came to: `unfinished` or
 *     `incomplete` with the first REPLAY line of that state, `clean` or
 *     the counts of the kinds a replay reproduces, or `unjudged` and why,
 *     and whether it did not let go of memory. A summary closes:
 *     `replayed P, clean C, incomplete I, unfinished U`, how many of the
 *     judged were of version 1 and took inputs from the harness, how many
 *     records are not kept pages, and how many are unjudged, which makes
 *     the answer 2.
 *
 * --self-test is the proof (run by tools/test_yonder_replay_host.sh): the
 * fixture pages the scripted-page host's fixtures serve
 * (tools/yonder_loop_fixture, tools/yonder_script_fixture, tools/pages) and
 * hand-made ones are loaded with keep_pages on, as a person browsing would,
 * in a child of their own, their sub-resources served by the runner; what
 * yonder kept is written out to WORK-DIR/kept beside the records it wrote,
 * replayed into WORK-DIR/out by the path above, and each replayed record
 * must be the page's own (rp_same: both were read in this harness, which
 * answers no picture or font either way). Then copies of one, and pages
 * written here, are replayed as corpora that must not pass silently
 * (rp_corpus_cases), each refusal checked by the reason its one rule
 * gives; tools/test_yonder_replay_mutants.py breaks each rule in turn and
 * requires this proof to fail.
 */
#define YONDER_HOST_FAKES_ONLY
#pragma GCC diagnostic ignored "-Wunused-function"
#include "test_yonder_scripts_host.c"

#include <ctype.h>
#include <limits.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
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

/* THE HEAD FACTS (REPLAY.md § 3): the record's facts a reader parses, and
 * the inputs a replay reads the page with. */
enum { F_STATUS, F_FINAL, F_BODY, F_TYPE, F_CHARSET, F_ENCRYPTED, F_TLS, F_SCRIPTS, F_LAID, F_KEPT, F_BYTES,
       F_KEPT_LINES, F_MODERN, F_ZOOM, F_APPEARANCE, F_MOTION, F_SCRIPT_SECONDS, F_ANIMATIONS, F_AGENT,
       F_COOKIES, F_CLOCK, F_TZ, F_N };
static const char *const kFacts[F_N]={"status","final address","body","content-type","charset","encrypted",
    "TLS","scripts","laid out at","kept","bytes","kept lines","modern","zoom","appearance","motion",
    "script seconds","animate","agent","cookies","clock","tz"};
/* A page file as yonder writes it (diag.c): a kept page's, or the one the
 * window writes in a replay. Its VERSION (REPLAY.md § 3) is 2 when it says
 * any of the inputs version 2 added (`agent:`, `cookies:`, `clock:`,
 * `tz:`, which yonder's claim writes first), so one that lost some of them
 * is a version 2 record without them, and 1 when it says none. */
typedef struct {
    char *address;          /* line one, decoded */
    char *facts[F_N];       /* by kFacts, decoded; NULL when absent */
    unsigned missing, failed;   /* its record lines */
    unsigned replayed_missing, replayed_failed; /* those of the kinds a replay reproduces */
    const char *unjudged;   /* why the record says it cannot be judged; NULL when it does not */
    char *said;             /* what its `REPLAY unjudged:` line says, which `unjudged` may point at */
    bool ended;             /* its last line is YONDER_DIAG_END */
} RpRecord;

static void rp_record_free(RpRecord *r) {
    free(r->address); free(r->said);
    for(int i=0;i<F_N;i++) free(r->facts[i]);
    memset(r,0,sizeof(*r));
}
static int rp_version(const RpRecord *r) {
    return r->facts[F_AGENT]!=NULL || r->facts[F_COOKIES]!=NULL || r->facts[F_CLOCK]!=NULL || r->facts[F_TZ]!=NULL ? 2 : 1;
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
/* THE ONE NUMBER PARSER (REPLAY.md § 2): `n` bytes that are a whole
 * decimal, ASCII digits and nothing else, no leading zero past a lone `0`,
 * at most `limit`: UINT32_MAX, which no number diag.c writes passes (a
 * count, a byte count, an index line's number, a zoom), or RP_CLOCK_MAX
 * for a clock's milliseconds. False otherwise, however long.
 * tools/yonder_records.py's decimal() is the same rule. */
#define RP_CLOCK_MAX ((UINT64_C(1)<<53)-1)
static bool rp_decimal_to(const char *s, size_t n, uint64_t limit, uint64_t *v) {
    if(n==0 || n>(limit==UINT32_MAX ? 10u : 16u)) return false;
    if(n>1 && s[0]=='0') return false;
    uint64_t x=0;
    for(size_t i=0;i<n;i++) {
        if(s[i]<'0' || s[i]>'9') return false;
        x=x*10+(uint64_t)(s[i]-'0');
    }
    if(x>limit) return false;
    if(v!=NULL) *v=x;
    return true;
}
static bool rp_decimal(const char *s, size_t n, uint32_t *v) {
    uint64_t x=0;
    if(!rp_decimal_to(s,n,UINT32_MAX,&x)) return false;
    if(v!=NULL) *v=(uint32_t)x;
    return true;
}
/* The digits at `s` (up to `end`): how many. */
static size_t rp_digit_run(const char *s, const char *end) {
    size_t n=0;
    while(s+n<end && s[n]>='0' && s[n]<='9') n++;
    return n;
}
static bool rp_is(const char *s, size_t n, const char *word) { return strlen(word)==n && !memcmp(s,word,n); }
static bool rp_starts(const char *s, size_t n, const char *head) { return n>=strlen(head) && !memcmp(s,head,strlen(head)); }
/* Where the `m` bytes at `needle` first stand in the `n` at `s`; NULL for
 * nowhere. */
static const char *rp_find(const char *s, size_t n, const char *needle, size_t m) {
    for(size_t i=0;m<=n && i<=n-m;i++) if(!memcmp(s+i,needle,m)) return s+i;
    return NULL;
}

/* ONE LINE CLASSIFIED BY THE GRAMMAR (tools/yonder_record_grammar.txt holds
 * its vectors, which this reader and tools/yonder_records.py's classify()
 * both run and must agree on): the kind's name, or "REFUSED" /
 * "index:REFUSED" with `why`. A line that begins like a kind and does not
 * match its grammar exactly is REFUSED, never read as some other kind.
 * The fields a reader takes: `a`/`b` (MISSING's kind and name, FAILED's
 * what and message, a fact's key and value, a REPLAY line's text), `n`
 * (counts, an index entry's number and job), and an index entry's fields
 * by their places and lengths (`f`, `fl`, `nf`), never re-split by a
 * routine that stops at a NUL. */
typedef struct {
    const char *kind, *why;
    const char *a, *b; size_t alen, blen;
    uint32_t n[3];
    bool unwritten;
    const char *f[7]; size_t fl[7]; int nf;
} RpLine;

/* WHAT A REPLAY OF A VERSION 1 PAGE TAKES FROM OUTSIDE ITS RECORD
 * (REPLAY.md § 3), never silently. DEFAULTED, from the harness, named on
 * its `REPLAY inputs defaulted:` line in this order: the cookies and the
 * clock, which nobody can state for a page recorded before them. STATED,
 * by the person running the replay (`--window KEY=VALUE,...`), each with
 * the fact it stands for, named with its value on its `REPLAY inputs
 * stated:` line in this order: the window's inputs, the agent and the
 * zone, each taken only when the record does not say it. A version 1
 * record missing one that is not stated is unjudged.
 * tools/yonder_records.py's DEFAULTABLE and STATABLE are the same sets. */
static const char *const kDefaultable[]={"cookies","clock"};
#define RP_DEFAULTABLE (sizeof(kDefaultable)/sizeof(kDefaultable[0]))
static const struct { const char *key; int fact; } kStatable[]={
    {"zoom",F_ZOOM},{"appearance",F_APPEARANCE},{"motion",F_MOTION},{"animate",F_ANIMATIONS},{"modern",F_MODERN},
    {"scripts",F_SCRIPTS},{"script-seconds",F_SCRIPT_SECONDS},{"laid-out-at",F_LAID},{"agent",F_AGENT},{"tz",F_TZ}};
#define RP_STATABLE (sizeof(kStatable)/sizeof(kStatable[0]))
/* What `--window` states, by kStatable, as the fact's value (`laid-out-at`
 * as `<W>x<H> px`); NULL for what it does not. */
static char *rp_stated[RP_STATABLE];
/* Where a page's inputs came from besides its record: `defaulted` and
 * `stated` by kDefaultable and kStatable, and `assumed`, a version 1 page
 * whose record predates `kept:` taken as kept whole (rp_is_kept). */
typedef struct { bool defaulted[RP_DEFAULTABLE], stated[RP_STATABLE], assumed; } RpFrom;
#define RP_KEPT_ASSUMED "REPLAY kept: assumed whole (version 1)"

/* A fact value a reader parses, matched whole: NULL, or why it is not. */
static const char *rp_head_value(const char *key, size_t klen, const char *v, size_t n) {
    static const char *const kDecimal[]={"bytes","kept lines","zoom","script seconds"};
    if(rp_is(key,klen,"clock")) return rp_decimal_to(v,n,RP_CLOCK_MAX,NULL) ? NULL : "a count no record holds";
    if(rp_is(key,klen,"agent") || rp_is(key,klen,"tz")) return n>0 ? NULL : "a value yonder does not write";
    static const struct { const char *key, *a, *b, *c; } kWords[]={
        {"body","html","text","image"},{"encrypted","yes","no",NULL},{"scripts","on","off",NULL},
        {"modern","on","off",NULL},{"appearance","light","dark",NULL},{"motion","full","reduce",NULL},
        {"animate","on","off",NULL}};
    const char *end=v+n;
    for(size_t i=0;i<sizeof(kDecimal)/sizeof(kDecimal[0]);i++)
        if(rp_is(key,klen,kDecimal[i])) return rp_decimal(v,n,NULL) ? NULL : "a count no record holds";
    for(size_t i=0;i<sizeof(kWords)/sizeof(kWords[0]);i++)
        if(rp_is(key,klen,kWords[i].key))
            return rp_is(v,n,kWords[i].a) || rp_is(v,n,kWords[i].b) || (kWords[i].c!=NULL && rp_is(v,n,kWords[i].c))
                 ? NULL : "a value yonder does not write";
    if(rp_is(key,klen,"status")) {
        size_t d=rp_digit_run(v,end);
        if(d==0 || (d<n && v[d]!=' ')) return "a value yonder does not write";
        return rp_decimal(v,d,NULL) ? NULL : "a count no record holds";
    }
    if(rp_is(key,klen,"kept"))
        return rp_is(v,n,"whole") || (rp_starts(v,n,"not whole: ") && n>11) ? NULL : "a value yonder does not write";
    if(rp_is(key,klen,"laid out at")) {
        /* W[xH] px[ in N ms] */
        const char *p=v; size_t d=rp_digit_run(p,end);
        if(d==0) return "a value yonder does not write";
        if(!rp_decimal(p,d,NULL)) return "a count no record holds";
        p+=d;
        if(p<end && *p=='x') {
            p++; d=rp_digit_run(p,end);
            if(d==0) return "a value yonder does not write";
            if(!rp_decimal(p,d,NULL)) return "a count no record holds";
            p+=d;
        }
        if(!rp_starts(p,(size_t)(end-p)," px")) return "a value yonder does not write";
        p+=3;
        if(p==end) return NULL;
        if(!rp_starts(p,(size_t)(end-p)," in ")) return "a value yonder does not write";
        p+=4; d=rp_digit_run(p,end);
        if(d==0 || !rp_is(p+d,(size_t)(end-p-d)," ms")) return "a value yonder does not write";
        return rp_decimal(p,d,NULL) ? NULL : "a count no record holds";
    }
    return NULL;    /* final address, content-type, charset, TLS, cookies: any value */
}
static bool rp_is_head(const char *key, size_t klen) {
    for(int i=0;i<F_N;i++) if(rp_is(key,klen,kFacts[i])) return true;
    return false;
}

static RpLine rp_classify(const char *line, size_t len, bool index) {
    RpLine l; memset(&l,0,sizeof(l));
    const char *end=line+len;
    l.kind=index ? "index:REFUSED" : "REFUSED";
    /* A CONTROL BYTE ANYWHERE (a carriage return, a NUL, any byte below
     * 0x20 but the tab) is in no line of either: the escape spells every
     * one a value holds, so a raw one is damage, and a record or an index
     * holding one is refused whole, since its readers classify every line. */
    for(size_t i=0;i<len;i++)
        if((unsigned char)line[i]<0x20 && line[i]!='\t') { l.why="a control byte"; return l; }
    /* Nor one spelled `\x00`: a field that decodes to a NUL would read as
     * shorter than it is wherever a string ends at one. And every field
     * must decode to UTF-8, as the file must be written in it: an escape
     * may spell a byte that may not stand raw, never one that is no part
     * of a character (`agent: \xFF` would be an input no record holds). */
    size_t dlen=0;
    char *decoded=rp_unescape_n(line,len,!index,&dlen);
    bool nul=memchr(decoded,'\0',dlen)!=NULL, utf8=rp_utf8((const unsigned char *)decoded,dlen);
    free(decoded);
    if(nul) { l.why="a NUL in a field"; return l; }
    if(!utf8) { l.why="a field that escapes to bytes that are not UTF-8"; return l; }
    if(index) {
        if(rp_starts(line,len,"# yonder kept index ")) {
            if(rp_decimal(line+20,len-20,&l.n[0])) l.kind="index:version";
            else l.why="a count no record holds";
            return l;
        }
        if(len>0 && line[0]=='#') { l.kind="index:comment"; return l; }
        /* A script's or a sheet's line: six fields (version 1), or seven,
         * the job's identity last (version 2). */
        for(const char *p=line; l.nf<8; ) {
            const char *t=memchr(p,'\t',(size_t)(end-p));
            if(l.nf==7) { l.nf++; break; }
            l.f[l.nf]=p; l.fl[l.nf]=t!=NULL ? (size_t)(t-p) : (size_t)(end-p); l.nf++;
            if(t==NULL) break;
            p=t+1;
        }
        l.why="a line that is not a script's or a sheet's whole";
        if((l.nf!=6 && l.nf!=7) || !(rp_is(l.f[1],l.fl[1],"script") || rp_is(l.f[1],l.fl[1],"sheet")) || l.fl[2]==0 ||
           !(rp_is(l.f[3],l.fl[3],"came") || rp_is(l.f[3],l.fl[3],"failed")))
            return l;
        if(!rp_decimal(l.f[0],l.fl[0],&l.n[0]) || (l.nf==7 && !rp_decimal(l.f[6],l.fl[6],&l.n[1]))) {
            l.why="a count no record holds"; return l;
        }
        if(l.n[0]<1) return l;
        l.kind=l.nf==7 ? "index:entry" : "index:entry-v1"; l.why=NULL;
        l.a=l.f[2]; l.alen=l.fl[2];
        return l;
    }
    if(rp_is(line,len,YONDER_DIAG_END)) { l.kind="end"; return l; }
    /* `REPLAY unjudged: <why>`: a record yonder wrote saying it cannot be
     * judged (its lines lost, a setting changed during its load). */
    if(rp_starts(line,len,YONDER_DIAG_UNJUDGED) && len>strlen(YONDER_DIAG_UNJUDGED)) {
        l.kind="marked"; l.a=line+strlen(YONDER_DIAG_UNJUDGED); l.alen=len-strlen(YONDER_DIAG_UNJUDGED);
        return l;
    }
    /* `REPLAY inputs defaulted: <names>`: what a replay of a version 1 page
     * took from the harness, each name once, from kDefaultable. */
    if(rp_starts(line,len,"REPLAY inputs defaulted: ")) {
        const char *p=line+25;
        bool seen[RP_DEFAULTABLE]={false};
        l.why="a REPLAY inputs defaulted line naming no input a replay defaults";
        for(const char *w=p; ; ) {
            const char *sp=memchr(w,' ',(size_t)(end-w));
            size_t n=sp!=NULL ? (size_t)(sp-w) : (size_t)(end-w);
            size_t k=0;
            while(k<RP_DEFAULTABLE && !rp_is(w,n,kDefaultable[k])) k++;
            if(k==RP_DEFAULTABLE || seen[k]) return l;
            seen[k]=true;
            if(sp==NULL) break;
            w=sp+1;
        }
        l.kind="defaulted"; l.why=NULL; l.a=p; l.alen=(size_t)(end-p);
        return l;
    }
    /* `REPLAY inputs stated: <key>=<value> ...`: what the person said a
     * version 1 page's window was, each key once, from kStatable, each
     * value escaped (a space in it spelled `\x20`) and not empty. */
    if(rp_starts(line,len,"REPLAY inputs stated: ")) {
        const char *p=line+22;
        bool seen[RP_STATABLE]={false};
        l.why="a REPLAY inputs stated line naming no input a person states, or none twice";
        for(const char *w=p; ; ) {
            const char *sp=memchr(w,' ',(size_t)(end-w));
            size_t n=sp!=NULL ? (size_t)(sp-w) : (size_t)(end-w);
            const char *eq=memchr(w,'=',n);
            if(eq==NULL || eq+1==w+n) return l;
            size_t k=0;
            while(k<RP_STATABLE && !rp_is(w,(size_t)(eq-w),kStatable[k].key)) k++;
            if(k==RP_STATABLE || seen[k]) return l;
            seen[k]=true;
            if(sp==NULL) break;
            w=sp+1;
        }
        l.kind="stated"; l.why=NULL; l.a=p; l.alen=(size_t)(end-p);
        return l;
    }
    if(rp_is(line,len,RP_KEPT_ASSUMED)) { l.kind="assumed"; return l; }
    /* `REPLAY budget: <seconds> s`: the virtual time the replay had. */
    if(rp_starts(line,len,"REPLAY budget: ")) {
        l.why="a REPLAY budget of no spelling a replay writes";
        if(len<17 || !rp_is(end-2,2," s") || !rp_decimal(line+15,len-17,&l.n[0])) return l;
        l.kind="budget"; l.why=NULL;
        return l;
    }
    if(rp_starts(line,len,"address: ")) { l.kind="address"; l.a=line+9; l.alen=len-9; return l; }
    if(rp_starts(line,len,"verdict: ")) {
        const char *p=line+9; size_t rest=len-9, d;
        l.why="a verdict of no spelling yonder writes";
        if(rp_is(p,rest,"clean")) { l.kind="verdict"; return l; }
        /* The claim's placeholder: `unwritten`, then its nonce (sixteen
         * lowercase hex digits) when the claim wrote one. */
        if(rp_is(p,rest,"unwritten") || (rest==26 && rp_starts(p,rest,"unwritten ") &&
                                         strspn(p+10,"0123456789abcdef")>=16)) {
            l.kind="verdict"; l.unwritten=true; return l;
        }
        d=rp_digit_run(p,end);
        if(d==0) return l;
        uint32_t a=0, b=0;
        const char *q=p+d;
        if(rp_is(q,(size_t)(end-q)," MISSING") || rp_is(q,(size_t)(end-q)," FAILED")) {
            if(!rp_decimal(p,d,&a)) { l.why="a count no record holds"; return l; }
            if(a==0) { l.why="a verdict that names a token with no records"; return l; }
            l.n[q[1]=='M' ? 0 : 1]=a; l.kind="verdict"; return l;
        }
        if(!rp_starts(q,(size_t)(end-q)," MISSING, ")) return l;
        const char *r=q+10; size_t e=rp_digit_run(r,end);
        if(e==0 || !rp_is(r+e,(size_t)(end-r-e)," FAILED")) return l;
        if(!rp_decimal(p,d,&a) || !rp_decimal(r,e,&b)) { l.why="a count no record holds"; return l; }
        if(a==0 || b==0) { l.why="a verdict that names a token with no records"; return l; }
        l.n[0]=a; l.n[1]=b; l.kind="verdict"; return l;
    }
    if(rp_starts(line,len,"MISSING ") || rp_starts(line,len,"FAILED ")) {
        bool missing=line[0]=='M';
        const char *p=line+(missing ? 8 : 7);
        l.why=missing ? "a record line with no count, or no kind and name" : "a record line with no count, or no what and message";
        /* The count: ` (<digits>)` at the line's end. */
        if(len<4 || line[len-1]!=')') return l;
        const char *close=end-1, *digits=close;
        while(digits>p && isdigit((unsigned char)digits[-1])) digits--;
        if(digits==close || digits-p<2 || digits[-1]!='(' || digits[-2]!=' ') return l;
        const char *suffix=digits-2;
        if(!rp_decimal(digits,(size_t)(close-digits),&l.n[0])) { l.why="a count no record holds"; return l; }
        if(missing) {
            const char *sp=memchr(p,' ',(size_t)(suffix-p));
            if(sp==NULL || sp==p || sp+1>=suffix) return l;
            l.a=p; l.alen=(size_t)(sp-p); l.b=sp+1; l.blen=(size_t)(suffix-sp-1);
            l.kind="missing"; return l;
        }
        const char *sep=NULL;
        for(const char *c=p+1; c+1<suffix; c++) if(c[0]==':' && c[1]==' ') { sep=c; break; }
        if(sep==NULL || sep+2>=suffix) return l;
        l.a=p; l.alen=(size_t)(sep-p); l.b=sep+2; l.blen=(size_t)(suffix-sep-2);
        l.kind="failed"; return l;
    }
    if(rp_starts(line,len,"REPLAY ")) {
        l.why="a REPLAY line of no kind this reader knows";
        if(rp_starts(line,len,"REPLAY state: ")) {
            static const char *const kWords[]={"verdict","incomplete","unfinished","unjudged"};
            const char *p=line+14;
            for(size_t i=0;i<4;i++) {
                size_t w=strlen(kWords[i]);
                if((size_t)(end-p)<w || memcmp(p,kWords[i],w)) continue;
                const char *q=p+w;
                if(q==end && i==0) { l.kind="state"; l.a=p; l.alen=w; return l; }
                if(q<end && *q==' ' && q+1<end && i!=0) { l.kind="state"; l.a=p; l.alen=w; l.b=q+1; l.blen=(size_t)(end-q-1); return l; }
                if(q<end && *q==' ' && i==0) { l.why="a REPLAY state with no why, or a verdict with one"; return l; }
                if(q==end && i!=0) { l.why="a REPLAY state with no why, or a verdict with one"; return l; }
            }
            return l;
        }
        if(rp_starts(line,len,"REPLAY incomplete ") && len>18) { l.kind="incomplete"; l.a=line+18; l.alen=len-18; return l; }
        if(rp_starts(line,len,"REPLAY unfinished ") && len>18) { l.kind="unfinished"; l.a=line+18; l.alen=len-18; return l; }
        if(rp_starts(line,len,"REPLAY not reproduced: ")) {
            const char *p=line+23;
            if(p==end) return l;
            for(const char *w=p; w<end; ) {
                const char *sp=memchr(w,' ',(size_t)(end-w));
                size_t n=sp!=NULL ? (size_t)(sp-w) : (size_t)(end-w);
                if(n==0 || (sp!=NULL && sp+1==end)) return l;
                if(rp_reproduced(w,n)) { l.why="a REPLAY not reproduced line naming what a replay does reproduce"; return l; }
                /* Each kind once, as each name on the inputs lines. */
                for(const char *v=p; v<w; ) {
                    size_t m=strcspn(v," ");
                    if(m==n && !memcmp(v,w,n)) { l.why="a REPLAY not reproduced line naming a kind twice"; return l; }
                    v+=m+1;
                }
                w+=n+(sp!=NULL);
            }
            l.kind="not-reproduced"; return l;
        }
        return l;
    }
    const char *sep=NULL;
    for(const char *c=line; c+1<end; c++) if(c[0]==':' && c[1]==' ') { sep=c; break; }
    l.why="a line of no kind a record holds";
    if(sep==NULL || sep==line) return l;
    l.a=line; l.alen=(size_t)(sep-line); l.b=sep+2; l.blen=(size_t)(end-sep-2);
    if(rp_is_head(l.a,l.alen)) {
        if((l.why=rp_head_value(l.a,l.alen,l.b,l.blen))!=NULL) return l;
        l.kind="head"; return l;
    }
    l.kind="fact"; l.why=NULL;
    return l;
}

/* A KEPT PAGE'S RECORD READ WHOLE, every line a kind of the grammar
 * (rp_classify), by the rules tools/yonder_records.py reads a record yonder
 * wrote by: NULL with `r` filled, or why it is not a whole record (`r`
 * empty). UTF-8; its first line `address: `, its second a verdict, its
 * record lines exactly the verdict's count, no other verdict line, no
 * other address and no head fact twice (a line a reader parses one value
 * from stands at most once: read first-wins, a `kept: whole` above a
 * `kept: not whole` would replay a page its own record refuses), no control
 * byte in any line (rp_classify refuses one, so
 * a carriage return or a NUL anywhere refuses the record), no `REPLAY`
 * line but a `REPLAY unjudged:` mark, and nothing past an `end` (`ended`):
 * a record that says its name was never written, or that it cannot be
 * judged, is read, and says why (`unjudged`). */
static const char *rp_record_read(const char *text, size_t size, RpRecord *r) {
    memset(r,0,sizeof(*r));
    if(size<9 || memcmp(text,"address: ",9)!=0) return "it is not a page file: its first line is not `address: `";
    if(!rp_utf8((const unsigned char *)text,size)) return "it is not a page file: it is not UTF-8";
    uint32_t want_missing=0, want_failed=0;
    unsigned line_no=0;
    const char *why=NULL;
    static char said[160];
    for(size_t at=0; at<size && why==NULL; line_no++) {
        const char *line=text+at, *end=memchr(line,'\n',size-at);
        size_t len=end!=NULL ? (size_t)(end-line) : size-at;
        at+=len+1;
        RpLine l=rp_classify(line,len,false);
        if(line_no==0) {
            if(strcmp(l.kind,"address")) {
                snprintf(said,sizeof(said),"it is not a whole page file: %s",l.why);
                why=said;
            } else r->address=rp_unescape(line+9,len-9,true);
        } else if(line_no==1) {
            if(strcmp(l.kind,"verdict")) {
                snprintf(said,sizeof(said),"it is not a whole page file: its second line is not a verdict%s%s%s",
                         l.why ? " (" : "",l.why ? l.why : "",l.why ? ")" : "");
                why=said;
            } else if(l.unwritten) r->unjudged="its name was claimed and its record never written";
            else { want_missing=l.n[0]; want_failed=l.n[1]; }
        } else if(!strcmp(l.kind,"missing") || !strcmp(l.kind,"failed")) {
            bool counted=rp_reproduced(l.a,l.alen);
            if(l.kind[0]=='m') { r->missing++; r->replayed_missing+=counted; }
            else { r->failed++; r->replayed_failed+=counted; }
        } else if(!strcmp(l.kind,"verdict")) {
            why="it is not a whole page file: a second verdict line";
        } else if(!strcmp(l.kind,"end")) {
            if(at<size) why="it is not a whole page file: lines past its `end`";
            r->ended=true;
        } else if(!strcmp(l.kind,"marked")) {
            /* The first reason it gives stands. */
            if(r->unjudged==NULL) {
                if(rp_is(l.a,l.alen,"lines lost")) r->unjudged="its record lost lines it could not keep";
                else {
                    char *text=rp_unescape(l.a,l.alen,true);
                    size_t n=strlen(text)+32;
                    r->said=malloc(n); snprintf(r->said,n,"its record says: %s",text);
                    free(text);
                    r->unjudged=r->said;
                }
            }
        } else if(!strcmp(l.kind,"head")) {
            for(int i=0;i<F_N && why==NULL;i++) {
                if(!rp_is(l.a,l.alen,kFacts[i])) continue;
                if(r->facts[i]!=NULL) {
                    snprintf(said,sizeof(said),"it is not a whole page file: a second `%s:`",kFacts[i]);
                    why=said;
                } else r->facts[i]=rp_unescape(l.b,l.blen,true);
            }
        } else if(!strcmp(l.kind,"fact")) {
            /* A plain fact may repeat: no reader parses one. */
        } else if(!strcmp(l.kind,"REFUSED")) {
            snprintf(said,sizeof(said),"it is not a whole page file: %s",l.why);
            why=said;
        } else if(!strcmp(l.kind,"address")) {
            why="it is not a whole page file: a second `address:`";
        } else {
            why="it is not a kept page's record: it carries a REPLAY line";
        }
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
typedef RpKnown (*rp_lookup_t)(void *ctx, uint32_t kind, const char *url, uint32_t id, RpAnswer *out);

/* The index of a kept page's folder (diag.h): its lines of the kinds this
 * runner answers, each with its file's bytes when it came, and how often
 * each was handed out, so the second job for one address is given the
 * second line kept for it. A job past the last has no line of its own: the
 * page asked more often than where it was kept, and an answer made up from
 * an earlier line would run a script or apply a sheet once more than the
 * page did, so it is unknown (incomplete). A line of any other shape is
 * damage (rp_index_read). A line of version 1 records no job identity
 * (`any_id`): it answers the first job of its kind and address that asks,
 * in the kept order. */
typedef struct { uint32_t kind, n, id; bool any_id, came; char *url, *final, *label; char *bytes; size_t len; unsigned used; } RpEntry;
typedef struct { char *folder; RpEntry *e; size_t n; unsigned lines; int version; } RpIndex;

static void rp_index_free(RpIndex *x) {
    for(size_t i=0;i<x->n;i++) { free(x->e[i].url); free(x->e[i].final); free(x->e[i].label); free(x->e[i].bytes); }
    free(x->e); free(x->folder); memset(x,0,sizeof(*x));
}
/* The folder's index, every line a kind of the grammar (rp_classify), and
 * the file of each line that came, read whole before the replay begins:
 * NULL, or why the page cannot be judged (its reason in `fault` when the
 * host said it). A page with NO FOLDER fetched nothing it kept, and its
 * index is empty (`version` 0); a folder whose index is not there or
 * cannot be read, is empty, or whose first line is not the version line
 * of a version this runner reads (REPLAY.md § 4: 1, six fields to a line,
 * or 2, seven, the job's identity last), whose lines are not each a
 * comment or a script's or a sheet's line of that version (the only lines
 * yonder writes: a slice that keeps another kind teaches this runner it
 * in the same change), whose lines are not numbered 1 to N in order, each
 * once, as yonder numbers them, or which names a file that is not there
 * or cannot be read, is a kept page with a piece missing. A line's number
 * names its file, and is a decimal and nothing else, so it can never name
 * a path outside the folder. */
static const char *rp_index_read(const char *folder, RpIndex *x, RpFault *fault) {
    memset(x,0,sizeof(*x));
    x->folder=strdup(folder);
    bool dir=false;
    RpFs got=rp_fs_kind(folder,&dir,fault);
    if(got==RP_FS_ABSENT) return NULL;
    if(got==RP_FS_FAULT) return fault->why;
    if(!dir) return "its folder is not a directory";
    char *path=rp_cat(folder,"/index","");
    char *text=NULL; size_t size=0;
    got=rp_fs_read(path,&text,&size,fault);
    free(path);
    if(got==RP_FS_ABSENT) return "its folder has no index";
    if(got==RP_FS_FAULT) return fault->why;
    static char why[160];
    const char *bad=size==0 ? "its index is empty" : NULL;
    size_t at=0; unsigned line_no=0;
    while(bad==NULL && at<size) {
        const char *line=text+at, *end=memchr(line,'\n',size-at);
        size_t len=end!=NULL ? (size_t)(end-line) : size-at;
        RpLine l=rp_classify(line,len,true);
        if(line_no++==0) {
            if(strcmp(l.kind,"index:version")) bad="its index is damaged: its first line is not its version";
            else if(l.n[0]!=1 && l.n[0]!=YONDER_DIAG_INDEX_VERSION) {
                snprintf(why,sizeof(why),"its index is of version %u, which this runner does not read",l.n[0]);
                bad=why;
            } else x->version=(int)l.n[0];
        } else if(!strcmp(l.kind,"index:entry") || !strcmp(l.kind,"index:entry-v1")) {
            x->lines++;
            if((x->version==1)!=(l.nf==6)) {
                snprintf(why,sizeof(why),"its index is damaged: a line of %d fields in an index of version %d",
                         l.nf,x->version);
                bad=why; break;
            }
            if(l.n[0]!=x->n+1) { bad="its index is damaged: its lines are not numbered 1 to N in order, each once"; break; }
            x->e=realloc(x->e,(x->n+1)*sizeof(*x->e));
            RpEntry *e=&x->e[x->n++]; memset(e,0,sizeof(*e));
            e->kind=rp_is(l.f[1],l.fl[1],"script") ? YONDER_JOB_SCRIPT : YONDER_JOB_SHEET;
            e->n=l.n[0]; e->id=l.n[1]; e->any_id=l.nf==6;
            e->url=rp_unescape(l.f[2],l.fl[2],false);
            e->came=rp_is(l.f[3],l.fl[3],"came");
            e->final=rp_unescape(l.f[4],l.fl[4],false);
            e->label=rp_unescape(l.f[5],l.fl[5],false);
            if(e->came) {
                char file[32]; snprintf(file,sizeof(file),"%u.%s",e->n,e->kind==YONDER_JOB_SCRIPT ? "js" : "css");
                char *file_at=rp_cat(folder,"/",file);
                got=rp_fs_read(file_at,&e->bytes,&e->len,fault);
                if(got==RP_FS_ABSENT) { snprintf(why,sizeof(why),"its index names %s, which is not there",file); bad=why; }
                else if(got==RP_FS_FAULT) bad=fault->why;
                free(file_at);
            }
        } else if(strcmp(l.kind,"index:comment")) {
            x->lines+=strcmp(l.kind,"index:version")!=0;
            snprintf(why,sizeof(why),"its index is damaged: %s",l.why!=NULL ? l.why : "a second version line");
            bad=why;
        }
        if(end==NULL) break;
        at+=len+1;
    }
    free(text);
    return bad;
}
/* Whether line `e` is the one kept for job `id` of `kind` at `url`: by
 * the job's own identity too (a script's token, a sheet's place in its
 * page's table), so two jobs of one kind and address each take the
 * outcome their own fetch had; a version 1 line records none. */
static bool rp_entry_is(const RpEntry *e, uint32_t kind, const char *url, uint32_t id) {
    return e->kind==kind && (e->any_id || e->id==id) && !strcmp(e->url,url);
}
/* The first line not yet taken that was kept for this job (rp_entry_is). */
static RpKnown rp_index_lookup(void *ctx, uint32_t kind, const char *url, uint32_t id, RpAnswer *out) {
    RpIndex *x=ctx; RpEntry *pick=NULL;
    for(size_t i=0;i<x->n && pick==NULL;i++)
        if(x->e[i].used==0 && rp_entry_is(&x->e[i],kind,url,id))
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
/* The identity a script's or a sheet's job carries, which the kept index
 * records beside its address: a script's token, a sheet's place in its
 * page's table. */
static uint32_t rp_job_id(const RpJob *j) {
    return j->kind==YONDER_JOB_SCRIPT ? ((const yonder_script_job_t *)j->work.job)->token
                                      : (uint32_t)((const yonder_sheet_job_t *)j->work.job)->index;
}
static bool rp_names(const RpEntry *e, const RpJob *j) { return rp_entry_is(e,j->kind,rp_job_url(j),rp_job_id(j)); }
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
        if(rp_order->e[i].used==0 && rp_names(&rp_order->e[i],j))
            return true;
    return false;
}
/* Whether job `j` may be answered now: a navigation's at once; a script's
 * or a sheet's when it is the job the kept order's next line names, or has
 * no line left at all (which makes it incomplete). */
static bool rp_turn_of(const RpJob *j) {
    if(j->kind==YONDER_JOB_TRIP || rp_order==NULL) return true;
    const RpEntry *head=rp_next_line();
    if(head!=NULL && rp_names(head,j)) return true;
    return !rp_line_left(j);
}
/* One round trip of the network: every job out when it began that the
 * replay answers and whose turn it is (rp_turn_of), in the order it was
 * submitted; a job whose line comes later waits for it. A job submitted
 * while answering waits for the next. A navigation's job comes back with
 * nothing: everything it had to say went down the mailbox. A script or a
 * sheet the source knows nothing of is not made up, neither as content
 * nor as a failure: it stays out, and the record says the replay was
 * incomplete (REPLAY.md § 4), so a page that asks for
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
            RpKnown known=rp_lookup(rp_lookup_ctx,j->kind,url,rp_job_id(j),&a);
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
    /* THE WINDOW the page was read in, as its record says (rp_window_of;
     * a version 1 page's the harness's where its record says none), every
     * one set on the replay's window. */
    int32_t width, height;          /* the view's */
    uint32_t zoom;                  /* per mille */
    bool dark, reduce, still, modern, scripts;    /* `still`: animations off */
    uint32_t script_seconds;
    /* THE PAGE'S OTHER INPUTS (REPLAY.md § 3): the agent its requests and
     * navigator name, the cookies a script could read, the clock Date
     * starts from (epoch milliseconds) and the zone it reads local time
     * in (a TZ string, or `<n>m` minutes east when yonder had none; NULL
     * leaves the harness's). */
    const char *agent, *cookies, *tz;
    uint64_t clock_ms;
    const uint8_t *bytes; size_t len;
    const char *dir;                /* where on the fake disk the window writes */
    bool keep;                      /* keep_pages, as a person browsing has it */
    uint64_t budget_ms;
} RpLoad;
typedef enum { RP_QUIET, RP_SPENT, RP_BUSY, RP_NAVIGATED } RpEnd;
typedef struct { RpEnd end; bool arrived; unsigned harness_failures; size_t live_after; } RpRan;

/* The inputs the harness would give a page that names none: its agent,
 * the cookies its fake jar hands a script, its clock, and its own zone.
 * A self-test's page is loaded with them, and a replay of a version 1
 * page takes each its record lacks (rp_window_of). */
static void rp_harness_inputs(RpLoad *load) {
    load->agent=YONDER_AGENT; load->cookies=fake_cookie_read; load->tz=NULL;
    load->clock_ms=(uint64_t)js_test_clock.epoch*1000+
                   (uint64_t)js_test_clock.ticks_into_second*1000/js_test_clock.ticks_per_second;
}
/* A page's inputs set where the window and its scripts read them: the
 * agent on the session, the cookies in the jar's script door, the clock
 * the JS Date origin reads (to the millisecond), and the zone: a TZ
 * string into the environment os64_getenv reads, or `<n>m`, the machine's
 * configured offset yonder recorded when it had no TZ, with TZ unset. */
static void rp_apply_inputs(const RpLoad *load) {
    g.way.agent=load->agent;
    fake_cookie_read=load->cookies;
    js_test_clock.epoch=(int64_t)(load->clock_ms/1000);
    js_test_clock.ticks_per_second=1000;
    js_test_clock.ticks_into_second=(uint32_t)(load->clock_ms%1000);
    if(load->tz==NULL) return;
    size_t n=strlen(load->tz);
    bool west=n>0 && load->tz[0]=='-';
    uint32_t minutes=0;
    if(n>=2 && load->tz[n-1]=='m' && rp_decimal(load->tz+west,n-1-west,&minutes) && minutes<=24*60) {
        js_test_clock.tz_offset_minutes=west ? -(int32_t)minutes : (int32_t)minutes;
        unsetenv("TZ");
    } else setenv("TZ",load->tz,1);
}

static bool rp_quiet(void) {
    if(g.stream.active || g.coming.active || rp_answerable()) return false;
    return !yonder_scripts_alive(g.page.scripts) || yonder_scripts_timer_next(g.page.scripts)==UINT64_MAX;
}
/* The loop turned as main turns it when nobody touches the page (no input
 * is dispatched: REPLAY.md § 5), the clock moved a tick at a time when
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
    g.scripts_on=load->scripts; g.script_ms=(uint64_t)load->script_seconds*1000;
    g.modern=load->modern; g.zoom=load->zoom; s_dark=load->dark; s_reduce_motion=load->reduce;
    s_animations=!load->still;
    rp_apply_inputs(load);
    layout();
    g.root.bounds.w+=load->width-g.view.bounds.w;
    g.root.bounds.h+=load->height-g.view.bounds.h;
    layout();
    memset(&s_diag,0,sizeof(s_diag));
    os64_strcopy(s_diag.dir,sizeof(s_diag.dir),load->dir);
    s_diag.next=load->seq; s_diag.keep=load->keep;
    now_ms=100; clock_us=10000000; clock_step=RP_MICROS_STEP;
    if(load->local) {
        slurp_path=load->address+strlen("file://"); slurp_bytes=(const char *)load->bytes; slurp_length=load->len;
        if(load->len==0) slurp_bytes="";
        start_trip(load->address,NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
        slurp_path=NULL; slurp_bytes=NULL; slurp_length=0;
    } else {
        start_trip(load->address,NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
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
 * REPLAY line of its state says), for a VERDICT the window record's lines
 * of the kinds a replay reproduces, and, for a judged page of version 1,
 * where its inputs came from besides its record (`from`). */
typedef struct {
    RpState state;
    char why[1100];
    unsigned missing, failed;
    RpFrom from;
    uint32_t budget_s;          /* the virtual seconds the replay was given */
} RpOutcome;

/* THE ONE PLACE A PAGE'S STATE IS DECIDED, from the page's own run, never
 * from a record read back: UNJUDGED when `unjudged` names why (the page
 * could not be read or replayed, a harness check failed, the window's
 * record lost lines or is not whole, or its record could not be written);
 * else UNFINISHED when the replay reached no verdict, INCOMPLETE when the
 * page and its kept fetches did not meet one for one; else VERDICT, with
 * the window's record lines of the kinds a replay reproduces (`window`),
 * none of which is clean. */
static RpOutcome rp_decide(const char *unjudged, const RpRecord *window, const RpFrom *from) {
    RpOutcome o; memset(&o,0,sizeof(o));
    if(unjudged!=NULL) {
        o.state=RP_UNJUDGED;
        snprintf(o.why,sizeof(o.why),"%s",unjudged);
        return o;
    }
    o.state=rp_unfinished ? RP_UNFINISHED : rp_incomplete ? RP_INCOMPLETE : RP_VERDICT;
    if(o.state!=RP_VERDICT) snprintf(o.why,sizeof(o.why),"%s",rp_first[o.state]);
    o.missing=window->replayed_missing; o.failed=window->replayed_failed;
    if(from!=NULL) o.from=*from;
    return o;
}
/* Whether a judged page took any input from the harness. */
static bool rp_any_taken(const RpOutcome *o) {
    for(size_t k=0;k<RP_DEFAULTABLE;k++) if(o->from.defaulted[k]) return true;
    return false;
}

/* THE RECORD A REPLAY WRITES, the page's state IN IT: its address; `REPLAY
 * state: <state> <why>` as its second line, before any token line; for a
 * judged state the window's record from its verdict on, the replay's own
 * REPLAY lines, for a version 1 page what it took from outside its record
 * (`REPLAY inputs stated:`, `REPLAY inputs defaulted:`, `REPLAY kept:
 * assumed whole (version 1)`), the virtual seconds it was given (`REPLAY
 * budget:`, which the diff compares), and the kinds it does not
 * reproduce; and `end`, which both
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
        /* The window's record without its own `end`: the replay's lines
         * follow it, and the record's one `end` closes them. */
        const char *body=nl+1; size_t blen=wlen-(size_t)(body-window);
        if(blen>=4 && !memcmp(body+blen-4,YONDER_DIAG_END "\n",4) && (blen==4 || body[blen-5]=='\n')) blen-=4;
        rp_add(&t,body,blen);
        if(rp_said.n) rp_add(&t,rp_said.p,rp_said.n);
        bool stated=false;
        for(size_t k=0;k<RP_STATABLE;k++) {
            if(!o->from.stated[k]) continue;
            rp_add(&t,stated ? " " : "REPLAY inputs stated: ",stated ? 1 : 22);
            rp_add(&t,kStatable[k].key,strlen(kStatable[k].key)); rp_add(&t,"=",1);
            /* The value as a page file writes data, its spaces `\x20` so
             * the line splits at the spaces between its items alone. */
            RpText value={0}; rp_add_escaped(&value,rp_stated[k]);
            for(size_t i=0;i<value.n;i++)
                if(value.p[i]==' ') rp_add(&t,"\\x20",4); else rp_add(&t,value.p+i,1);
            free(value.p);
            stated=true;
        }
        if(stated) rp_add(&t,"\n",1);
        if(rp_any_taken(o)) {
            rp_add(&t,"REPLAY inputs defaulted:",24);
            for(size_t k=0;k<RP_DEFAULTABLE;k++)
                if(o->from.defaulted[k]) { rp_add(&t," ",1); rp_add(&t,kDefaultable[k],strlen(kDefaultable[k])); }
            rp_add(&t,"\n",1);
        }
        if(o->from.assumed) rp_add(&t,RP_KEPT_ASSUMED "\n",strlen(RP_KEPT_ASSUMED)+1);
        /* The budget the page was given, which the diff compares: two
         * replays given different time are two different readings. */
        char budget[48]; snprintf(budget,sizeof(budget),"REPLAY budget: %u s\n",o->budget_s);
        rp_add(&t,budget,strlen(budget));
        rp_add(&t,kNot,sizeof(kNot)-1);
    }
    rp_add(&t,"end\n",4);
    return t;
}

/* ── Replaying a directory ──────────────────────────────────────────── */

/* What a directory's replay came to: how many pages came to each state,
 * how many of the judged were clean, how many of the judged were of
 * version 1 and took inputs from the harness (`defaulted`), and, beside
 * them, the records that are not kept pages (rp_is_kept), which are
 * passed over, and the pages that did not let go of what they held. An
 * UNJUDGED page makes the run's answer 2, since a corpus with pages nobody
 * judged reads as one that passed. */
typedef struct { unsigned states[RP_STATES], clean, bare, leaked, defaulted; } RpTally;

/* The record's sequence, from its file name `<anything>-<digits>.txt`,
 * whose digits yonder pads with zeros (diag.c's `%04lu`): the padding
 * passed over, the rest through the one number parser. 0 when the name is
 * not of that shape. */
static uint32_t rp_seq(const char *name) {
    size_t n=strlen(name);
    if(n<6 || strcmp(name+n-4,".txt")) return 0;
    size_t at=n-4; while(at>0 && name[at-1]>='0' && name[at-1]<='9') at--;
    if(at==n-4 || at==0 || name[at-1]!='-') return 0;
    while(at<n-5 && name[at]=='0') at++;
    uint32_t seq=0;
    return rp_decimal(name+at,n-4-at,&seq) ? seq : 0;
}

/* `s` as it may reach a terminal, by tools/yonder_records.py's display()
 * rule: a control character (C0, DEL and C1) re-spelled `\xHH`, a
 * backslash `\\`, and a byte that is no part of UTF-8 `\xHH` too, so text
 * a page wrote (an address, a fact the runner quotes, a file's name) can
 * neither drive the terminal nor pass for the runner's own words. Every
 * such string the runner prints goes through it. The answer lives until
 * the eighth call after. */
static const char *rp_display(const char *s) {
    static char ring[8][4096]; static unsigned turn;
    char *out=ring[turn++%8]; size_t o=0;
    const unsigned char *u=(const unsigned char *)s;
    for(size_t i=0; u[i] && o+9<sizeof(ring[0]); ) {
        unsigned c=u[i]; size_t n=c<0x80 ? 1 : c>=0xC2 && c<=0xDF ? 2 : c>=0xE0 && c<=0xEF ? 3 : c>=0xF0 && c<=0xF4 ? 4 : 0;
        if(n>1 && !rp_utf8(u+i,strnlen((const char *)u+i,n))) n=0;
        if(n==0) { o+=(size_t)snprintf(out+o,sizeof(ring[0])-o,"\\x%02X",c); i++; continue; }
        unsigned cp=n==1 ? c : n==2 ? ((c&0x1F)<<6)|(u[i+1]&0x3F) : 0x100;
        if(cp<0x20 || (cp>=0x7F && cp<0xA0)) o+=(size_t)snprintf(out+o,sizeof(ring[0])-o,"\\x%02X",cp);
        else if(c=='\\') { out[o++]='\\'; out[o++]='\\'; }
        else { memcpy(out+o,u+i,n); o+=n; }
        i+=n;
    }
    out[o]='\0';
    return out;
}

/* What the page came to, counted and printed on its line: the state, and
 * why or its counts. */
static void rp_report(const char *name, const RpOutcome *o, bool leaked, RpTally *t) {
    t->states[o->state]++;
    switch(o->state) {
    case RP_UNJUDGED: printf("%s  unjudged: %s",rp_display(name),rp_display(o->why)); break;
    case RP_UNFINISHED:
        printf("%s  unfinished (%s)%s",rp_display(name),rp_display(o->why),rp_incomplete ? ", incomplete" : "");
        break;
    case RP_INCOMPLETE: printf("%s  incomplete (%s)",rp_display(name),rp_display(o->why)); break;
    case RP_VERDICT:
        if(o->missing==0 && o->failed==0) { t->clean++; printf("%s  clean",rp_display(name)); }
        else if(o->missing!=0 && o->failed!=0) printf("%s  %u MISSING, %u FAILED",rp_display(name),o->missing,o->failed);
        else printf("%s  %u %s",rp_display(name),o->missing ? o->missing : o->failed,o->missing ? "MISSING" : "FAILED");
        break;
    case RP_STATES: break;
    }
    if(leaked) { printf(" (it did not let go of what it held)"); t->leaked++; }
    printf("\n");
}

/* Whether `s` begins with a key of kStatable and its `=`. */
static bool rp_statable_at(const char *s) {
    for(size_t k=0;k<RP_STATABLE;k++) {
        size_t n=strlen(kStatable[k].key);
        if(!strncmp(s,kStatable[k].key,n) && s[n]=='=') return true;
    }
    return false;
}
/* What is stated forgotten. */
static void rp_window_clear(void) {
    for(size_t k=0;k<RP_STATABLE;k++) { free(rp_stated[k]); rp_stated[k]=NULL; }
}
/* `--window KEY=VALUE,...` (REPLAY.md § 3): what the person running the
 * replay states a version 1 corpus's window was, into rp_stated. An item
 * ends at a comma that a key of kStatable and its `=` follow, so a value
 * may hold commas (an agent string does); each key once, each value one
 * the record's grammar takes for its fact (`laid-out-at` as `<W>x<H>`, a
 * width and a height). NULL, or why not. */
static const char *rp_window_state(const char *arg) {
    static char why[300];
    for(const char *p=arg; *p; ) {
        const char *eq=strchr(p,'=');
        size_t k=0;
        while(eq!=NULL && k<RP_STATABLE && !rp_is(p,(size_t)(eq-p),kStatable[k].key)) k++;
        if(eq==NULL || k==RP_STATABLE) {
            snprintf(why,sizeof(why),"--window: `%.40s` names no input a person states",p);
            return why;
        }
        if(rp_stated[k]!=NULL) { snprintf(why,sizeof(why),"--window: `%s` is stated twice",kStatable[k].key); return why; }
        const char *v=eq+1, *e=v;
        for(;;) {
            const char *c=strchr(e,',');
            if(c==NULL) { e=v+strlen(v); break; }
            if(rp_statable_at(c+1)) { e=c; break; }
            e=c+1;
        }
        size_t n=(size_t)(e-v);
        bool laid=kStatable[k].fact==F_LAID;
        char *value=malloc(n+4);
        snprintf(value,n+4,"%.*s%s",(int)n,v,laid ? " px" : "");
        const char *fact=kFacts[kStatable[k].fact];
        if(n==0 || rp_head_value(fact,strlen(fact),value,strlen(value))!=NULL ||
           (laid && v[rp_digit_run(v,e)]!='x')) {
            snprintf(why,sizeof(why),"--window: `%s=%.*s` is not a value a record says",kStatable[k].key,(int)n,v);
            free(value);
            return why;
        }
        rp_stated[k]=value;
        p=*e!='\0' ? e+1 : e;
    }
    return NULL;
}

/* THE PAGE'S INPUTS (REPLAY.md § 3), the window it was read in and its
 * network head, rebuilt from its record's facts: NULL, or why the page
 * cannot be judged. Every record says each one it needs, REQUIRED
 * (kNeeded), never assumed: its final address, body, kept lines, the view
 * it was laid out in (width and height), its zoom, appearance, motion,
 * whether animations run, the modern switch, scripts and their time
 * limit, its agent, the cookies a script could read, its clock and zone;
 * a page from the network its status, content-type, charset and
 * encryption too (a fact may be empty: the server named none). A record
 * of VERSION 1 is read by its own rules, each named in `from`: the
 * cookies and the clock, which it cannot say, are the harness's
 * (`defaulted`); the window's inputs, the agent and the zone it does not
 * say are what `--window` states (`stated`, rp_stated), and one neither
 * says nor states leaves the page unjudged; its `kept lines` binds the
 * index only when it says one. Each value has passed the grammar
 * (rp_classify). `index` is the page's kept index. */
static const char *rp_window_of(const RpRecord *r, bool local, const RpIndex *index, RpLoad *load, RpFrom *from) {
    static const int kNeeded[]={F_FINAL,F_BODY,F_KEPT_LINES,F_LAID,F_ZOOM,F_APPEARANCE,F_MOTION,F_ANIMATIONS,F_MODERN,
        F_SCRIPTS,F_SCRIPT_SECONDS,F_AGENT,F_COOKIES,F_CLOCK,F_TZ,F_STATUS,F_TYPE,F_CHARSET,F_ENCRYPTED};
    static char why[200], clock[32];
    const char *f[F_N];
    memcpy(f,r->facts,sizeof(f));
    bool v1=rp_version(r)<2;
    if(v1) {
        RpLoad harness; rp_harness_inputs(&harness);
        snprintf(clock,sizeof(clock),"%llu",(unsigned long long)harness.clock_ms);
        f[F_COOKIES]=harness.cookies; f[F_CLOCK]=clock;
        from->defaulted[0]=from->defaulted[1]=true;
        /* The version 1 builds wrote `content-type` and `charset` only
         * when the server named one (yonder.c at 08cf5de6), so a version 1
         * record without one says the server named none: that is its
         * format's spelling of empty, not an input taken from elsewhere. */
        if(!local && f[F_TYPE]==NULL) f[F_TYPE]="";
        if(!local && f[F_CHARSET]==NULL) f[F_CHARSET]="";
        /* A census-era `laid out at` says the page's width, not the view's
         * width and height: it says no view. */
        if(f[F_LAID]!=NULL && f[F_LAID][rp_digit_run(f[F_LAID],f[F_LAID]+strlen(f[F_LAID]))]!='x') f[F_LAID]=NULL;
        for(size_t i=0;i<RP_STATABLE;i++) {
            int k=kStatable[i].fact;
            if(f[k]!=NULL) continue;
            if(rp_stated[i]==NULL) {
                snprintf(why,sizeof(why),"its record does not say its `%s` and --window does not state it",kFacts[k]);
                return why;
            }
            f[k]=rp_stated[i];
            from->stated[i]=true;
        }
    }
    for(size_t i=0;i<sizeof(kNeeded)/sizeof(kNeeded[0]);i++)
        if(f[kNeeded[i]]==NULL && !(local && (kNeeded[i]==F_STATUS || kNeeded[i]==F_TYPE ||
                                              kNeeded[i]==F_CHARSET || kNeeded[i]==F_ENCRYPTED)) &&
           !(v1 && kNeeded[i]==F_KEPT_LINES)) {
            snprintf(why,sizeof(why),"its record does not say its `%s`",kFacts[kNeeded[i]]);
            return why;
        }
    /* `<width>x<height> px`, perhaps ` in <n> ms`. */
    const char *laid=f[F_LAID], *x=laid+rp_digit_run(laid,laid+strlen(laid)), *px=strstr(laid," px");
    if(*x!='x') x=NULL;
    uint32_t w=0, h=0;
    if(x==NULL || px==NULL || x>px || !rp_decimal(laid,(size_t)(x-laid),&w) || !rp_decimal(x+1,(size_t)(px-x-1),&h) ||
       w==0 || h==0)
        return "its record does not say the view's width and height";
    load->width=(int32_t)w; load->height=(int32_t)h;
    if(!rp_decimal(f[F_ZOOM],strlen(f[F_ZOOM]),&load->zoom) || load->zoom==0)
        return "its record's `zoom` is none yonder writes";
    if(!rp_decimal(f[F_SCRIPT_SECONDS],strlen(f[F_SCRIPT_SECONDS]),&load->script_seconds))
        return "its record's `script seconds` is none yonder writes";
    uint32_t want_lines=0;
    if(f[F_KEPT_LINES]!=NULL && (!rp_decimal(f[F_KEPT_LINES],strlen(f[F_KEPT_LINES]),&want_lines) ||
                                 want_lines!=index->lines)) {
        snprintf(why,sizeof(why),"its record says its index holds %u lines, and it holds %u",want_lines,index->lines);
        return why;
    }
    load->dark=!strcmp(f[F_APPEARANCE],"dark");
    load->reduce=!strcmp(f[F_MOTION],"reduce");
    load->still=!strcmp(f[F_ANIMATIONS],"off");
    load->modern=!strcmp(f[F_MODERN],"on");
    load->scripts=!strcmp(f[F_SCRIPTS],"on");
    load->agent=f[F_AGENT]; load->cookies=f[F_COOKIES]; load->tz=f[F_TZ];
    if(!rp_decimal_to(f[F_CLOCK],strlen(f[F_CLOCK]),RP_CLOCK_MAX,&load->clock_ms))
        return "its record's `clock` is none yonder writes";
    if(local) return NULL;
    way_head_t *hd=&load->head;
    const char *body=f[F_BODY], *sp=strchr(f[F_STATUS],' ');
    uint32_t status=0;
    if(!rp_decimal(f[F_STATUS],sp!=NULL ? (size_t)(sp-f[F_STATUS]) : strlen(f[F_STATUS]),&status))
        return "its record's `status` is none yonder writes";
    hd->status=(int)status;
    /* Copied into the buffers yonder wrote them from, so a value yonder
     * wrote fits whole. One longer was not written by yonder: the reader
     * bounds a record by its grammar and takes the rest as yonder's, which
     * is the row "The reader bounds a record by its grammar and nothing
     * else" in DECLINED.md. */
    os64_strcopy(hd->reason,sizeof(hd->reason),sp!=NULL ? sp+1 : "");
    os64_strcopy(hd->url,sizeof(hd->url),f[F_FINAL]);
    os64_strcopy(hd->content_type,sizeof(hd->content_type),f[F_TYPE]);
    os64_strcopy(hd->charset,sizeof(hd->charset),f[F_CHARSET]);
    hd->encrypted=!strcmp(f[F_ENCRYPTED],"yes");
    hd->body=!strcmp(body,"text") ? WAY_BODY_TEXT : !strcmp(body,"image") ? WAY_BODY_IMAGE : WAY_BODY_HTML;
    if(f[F_TLS]!=NULL) {
        hd->tls_version=!strncmp(f[F_TLS],"1.3",3) ? 0x0304 : 0x0303;
        hd->tls_fallback=strstr(f[F_TLS],"fallback")!=NULL;
    }
    return NULL;
}

/* Whether a whole record is a KEPT PAGE's (REPLAY.md § 3): one that says
 * `kept:` (whole or not), of either version, yonder writing it when the
 * page's bytes are done, last, so for a version 1 record, which may lack
 * `end`, it stands for that end. A version 1 record that predates `kept:`
 * (and says none) is kept when its page's bytes are beside it
 * (`<base>.html`), ASSUMED whole (`*assumed`): its `bytes:` must then
 * match them, and its `kept lines`, when it says one, its index, and the
 * replay's record says the assumption. */
static RpFs rp_is_kept(const char *base, const RpRecord *r, bool *kept, bool *assumed, RpFault *fault) {
    *kept=r->facts[F_KEPT]!=NULL; *assumed=false;
    if(*kept || rp_version(r)>=2) return RP_FS_OK;
    char *html=rp_cat(base,".html","");
    bool dir=false;
    RpFs got=rp_fs_kind(html,&dir,fault);
    free(html);
    if(got==RP_FS_FAULT) return got;
    *kept=*assumed=got==RP_FS_OK && !dir;
    return RP_FS_OK;
}
/* THE END RULE (REPLAY.md § 2): a kept page's record of version 2 (one
 * that says `kept lines:`, written at its claim, or `kept:`) ends with
 * `end`, so one cut short anywhere after its claim is never replayed. */
static bool rp_cut_short(const RpRecord *r) {
    return rp_version(r)>=2 && (r->facts[F_KEPT]!=NULL || r->facts[F_KEPT_LINES]!=NULL) && !r->ended;
}

/* What a page's child exits with (rp_one_process): RP_CHILD_BASE plus its
 * state, plus RP_CHILD_CLEAN for a clean verdict, RP_CHILD_LEAKED for a
 * page that did not let go of what it held, and RP_CHILD_DEFAULTED for a
 * judged page that took inputs from the harness: 64 to 95, above the
 * sanitizers' 1 and below nothing a shell or a signal makes. Any other
 * status is a child that died. */
enum { RP_CHILD_BASE=64, RP_CHILD_CLEAN=4, RP_CHILD_LEAKED=8, RP_CHILD_DEFAULTED=16, RP_CHILD_TOP=RP_CHILD_BASE+31 };

/* ONE KEPT PAGE REPLAYED, in the child forked for it: its record written
 * into `out` (`<out>/<name>`), its state in it, whole or not at all, and
 * its line printed; the status the child exits with returned. The parent
 * has decided it is a page to replay (rp_one_process): a record that is
 * not a kept page's reaches here only under a partial run, which replaces
 * an earlier record of it with one saying it is unjudged. */
static int rp_replay_page(const char *kept, const char *out, const char *name, uint64_t budget_ms) {
    char *stem=strndup(name,strlen(name)-4);
    char *base=rp_cat(kept,"/",stem);
    free(stem);
    RpFault fault;
    RpRecord r, window; memset(&r,0,sizeof(r)); memset(&window,0,sizeof(window));
    RpIndex index; memset(&index,0,sizeof(index));
    RpTally t; memset(&t,0,sizeof(t));
    RpFrom from; memset(&from,0,sizeof(from));
    char *text=NULL, *bytes=NULL;
    const char *unjudged=NULL, *record=NULL;
    char why[1100];
    size_t size=0, len=0, wlen=0;
    bool ran=false, leaked=false, is_kept=false;
    rp_said.n=0; rp_incomplete=0; rp_unfinished=false;
    char *path=rp_cat(base,".txt","");
    RpFs got=rp_fs_read(path,&text,&size,&fault);
    free(path);
    if(got!=RP_FS_OK) { unjudged=got==RP_FS_FAULT ? fault.why : "it is gone"; goto judge; }
    if((unjudged=rp_record_read(text,size,&r))!=NULL) goto judge;
    if((unjudged=r.unjudged)!=NULL) goto judge;
    if(rp_cut_short(&r)) { unjudged="its record has no `end`: it was cut short"; goto judge; }
    if(rp_is_kept(base,&r,&is_kept,&from.assumed,&fault)==RP_FS_FAULT) { unjudged=fault.why; goto judge; }
    if(!is_kept) { unjudged="it is not a kept page (its record does not say `kept:`)"; goto judge; }
    if(r.facts[F_KEPT]!=NULL && strcmp(r.facts[F_KEPT],"whole")) {
        snprintf(why,sizeof(why),"it was kept %s",r.facts[F_KEPT]);
        unjudged=why; goto judge;
    }
    path=rp_cat(base,".html","");
    got=rp_fs_read(path,&bytes,&len,&fault);
    free(path);
    if(got!=RP_FS_OK) { unjudged=got==RP_FS_FAULT ? fault.why : "its record says it was kept whole, and its bytes are gone"; goto judge; }
    /* NOTHING ON THE HOST BINDS A RECORD TO ITS KEPT FILES but what the
     * record says of them: the bytes the parser was fed, and the lines its
     * index holds (rp_window_of). Kept files that are not those are
     * another page's. Bytes of the same length swapped in under a record
     * pass, and so does a product of any length under a number its index
     * uses (an index line says no size); why neither is bound by content
     * is the row "Kept bytes bound to their record by length" in
     * DECLINED.md. */
    uint32_t said_bytes=0;
    if(r.facts[F_BYTES]==NULL || !rp_decimal(r.facts[F_BYTES],strlen(r.facts[F_BYTES]),&said_bytes) || said_bytes!=len) {
        snprintf(why,sizeof(why),"its record says the page had %s bytes, and its kept bytes are %zu",
                 r.facts[F_BYTES]!=NULL ? r.facts[F_BYTES] : "no count of",len);
        unjudged=why; goto judge;
    }
    path=rp_cat(base,".d","");
    unjudged=rp_index_read(path,&index,&fault);
    free(path);
    if(unjudged!=NULL) goto judge;
    /* A record and its index of one version: a page is read by one
     * version's rules, never the strict half of one and the defaults of
     * the other. */
    if(index.version!=0 && index.version!=rp_version(&r)) {
        snprintf(why,sizeof(why),"its record is of version %d and its index of version %d",rp_version(&r),index.version);
        unjudged=why; goto judge;
    }
    bool local=!strncmp(r.address,"file://",7);
    RpLoad load={0};
    if((unjudged=rp_window_of(&r,local,&index,&load,&from))!=NULL) goto judge;
    load.address=r.address; load.seq=rp_seq(name); load.bytes=(const uint8_t *)bytes; load.len=len;
    load.local=local;
    load.dir="/tmp/replay"; load.budget_ms=budget_ms;
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
    } else if(!window.ended) {
        unjudged="the window's own record has no `end`";
    } else if(window.unjudged!=NULL) {
        snprintf(why,sizeof(why),"the window's own record: %s",window.unjudged);
        unjudged=why;
    }
judge:;
    RpOutcome o=rp_decide(unjudged,&window,&from);
    o.budget_s=(uint32_t)(budget_ms/1000);
    RpText whole=rp_compose(&o,r.address,o.state==RP_UNJUDGED ? NULL : record,wlen);
    path=rp_cat(out,"/",name);
    if(rp_fs_write(path,whole.p,whole.n,&fault)!=RP_FS_OK && o.state!=RP_UNJUDGED) {
        /* The record could not be written whole: the page cannot be judged,
         * and that is written instead, if it can be. */
        snprintf(why,sizeof(why),"its record could not be written: %s",fault.why);
        o=rp_decide(why,&window,NULL);
        free(whole.p);
        whole=rp_compose(&o,r.address,NULL,0);
        (void)rp_fs_write(path,whole.p,whole.n,&fault);
    }
    free(path); free(whole.p);
    rp_report(name,&o,ran && leaked,&t);
    if(ran) fake_reset();
    free(text); free(bytes); free(base);
    rp_index_free(&index); rp_record_free(&r); rp_record_free(&window);
    return RP_CHILD_BASE+(int)o.state+(t.clean ? RP_CHILD_CLEAN : 0)+(t.leaked ? RP_CHILD_LEAKED : 0)+
           (o.state!=RP_UNJUDGED && rp_any_taken(&o) ? RP_CHILD_DEFAULTED : 0);
}

/* A child's end awaited, for at most rp_child_limit_ms of the wall clock
 * (REPLAY.md § 6: ten minutes; past it the child is killed): its status,
 * and whether it was killed for its time. The parent blocks in `waitpid`,
 * and a timer interrupts it every RP_WAIT_TICK_MS to look at the clock, so
 * an interrupted wait is the ordinary case: `waitpid` is retried when a
 * signal interrupted it, and on nothing else. False when the child cannot
 * be waited for. */
#define RP_CHILD_LIMIT_MS (10*60*1000)
#define RP_WAIT_TICK_MS 100
static uint64_t rp_child_limit_ms=RP_CHILD_LIMIT_MS;
static void rp_wait_tick(int sig) { (void)sig; }
/* The whole milliseconds from `from` to `to`, two readings of one clock:
 * the difference taken in signed nanoseconds, so a `to` whose nanoseconds
 * are below `from`'s borrows from the seconds instead of wrapping, and
 * 0 for a `to` before `from`. */
static uint64_t rp_elapsed_ms(const struct timespec *from, const struct timespec *to) {
    int64_t ns=((int64_t)to->tv_sec-(int64_t)from->tv_sec)*INT64_C(1000000000)+((int64_t)to->tv_nsec-(int64_t)from->tv_nsec);
    return ns>0 ? (uint64_t)ns/UINT64_C(1000000) : 0;
}
static bool rp_wait(pid_t pid, int *status, bool *timed_out) {
    struct sigaction tick, was;
    memset(&tick,0,sizeof(tick));
    tick.sa_handler=rp_wait_tick;      /* no SA_RESTART: the tick must interrupt the wait */
    sigemptyset(&tick.sa_mask);
    sigaction(SIGALRM,&tick,&was);
    struct itimerval every={{0,RP_WAIT_TICK_MS*1000},{0,RP_WAIT_TICK_MS*1000}}, off={{0,0},{0,0}};
    setitimer(ITIMER_REAL,&every,NULL);
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC,&start);
    *timed_out=false;
    bool waited=false;
    for(;;) {
        pid_t w=waitpid(pid,status,0);
        if(w==pid) { waited=true; break; }
        if(w>=0 || errno!=EINTR) break;
        clock_gettime(CLOCK_MONOTONIC,&now);
        if(!*timed_out && rp_elapsed_ms(&start,&now)>=rp_child_limit_ms) {
            kill(pid,SIGKILL);
            *timed_out=true;
        }
    }
    int saved=errno;
    setitimer(ITIMER_REAL,&off,NULL);
    sigaction(SIGALRM,&was,NULL);
    errno=saved;
    return waited;
}

/* ONE PROCESS PER PAGE (REPLAY.md § 6). The fakes the window runs on were
 * written for one-shot cases, and hold statics (the console's buffer, the
 * fake disk, the allocator's failure point, the pool fake's tables) that
 * one page's replay could leave for the next, which would then differ from
 * its kept run for a reason that is not its own. So each page is replayed
 * in a child forked for it (no thread runs before the fork: the pool is
 * run here, by hand), which writes its record through the door and exits
 * with what the page came to (RP_CHILD_*); the parent replays nothing and
 * holds no fake state.
 *
 * THE PARENT decides first, from the kept record, whether the page is one
 * to replay: a whole record that is not a kept page's is passed over
 * (`bare`), except under `replace` (a partial run) when OUT-DIR holds a
 * record of it, which is removed before the child starts, as is any
 * earlier record of a page a partial run replays, so a record that cannot
 * be written now leaves none rather than the old one; one that cannot be
 * removed refuses the run (false). Then it waits for the child (rp_wait),
 * removes the temporary the child's write would leave if it died in it
 * (`<out>/<name>.<pid>.new`), and TRUSTS the child's record only when its
 * status is in the table and the record agrees with it: its second line
 * the state the status names, its `REPLAY inputs defaulted:` line there
 * when the status says so, a verdict with no line of a kind a replay
 * reproduces exactly when the status says clean, and its `end`. A child that died, ran past its
 * time, exited outside the table, or whose record disagrees leaves its
 * page UNJUDGED, in a record the parent writes. */
/* The self-test's: a page whose child dies (`rp_crash_page`), never ends
 * (`rp_hang_page`), dies leaving its write's temporary (`rp_orphan_page`),
 * exits with a state its record does not say (`rp_lie_page`), with its
 * clean flag turned over (`rp_lie_clean_page`) or with a status outside
 * the table, as a sanitizer's death does (`rp_odd_exit_page`, exiting
 * `rp_odd_exit_code`), or dirties the fakes' statics after its record is
 * written (`rp_dirty_page`). */
static const char *rp_crash_page, *rp_hang_page, *rp_orphan_page, *rp_lie_page, *rp_lie_clean_page,
                  *rp_odd_exit_page, *rp_dirty_page;
static int rp_odd_exit_code=1;
static bool rp_one_process(const char *kept, const char *out, const char *name, uint64_t budget_ms, bool replace,
                           RpTally *t) {
    RpFault fault;
    char *at_out=rp_cat(out,"/",name);
    /* Whether the page is one to replay: a record that cannot be read
     * whole is (its child says why it cannot be judged). */
    {
        char *stem=strndup(name,strlen(name)-4), *base=rp_cat(kept,"/",stem), *path=rp_cat(base,".txt","");
        char *text=NULL; size_t size=0; RpRecord r; bool is_kept=true;
        if(rp_fs_read(path,&text,&size,&fault)==RP_FS_OK && rp_record_read(text,size,&r)==NULL) {
            bool k=false, assumed=false;
            if(r.unjudged==NULL && !rp_cut_short(&r) && rp_is_kept(base,&r,&k,&assumed,&fault)==RP_FS_OK) is_kept=k;
            rp_record_free(&r);
        }
        free(text); free(path); free(base); free(stem);
        bool there=false;
        if(!is_kept && replace) { bool dir=false; there=rp_fs_kind(at_out,&dir,&fault)!=RP_FS_ABSENT; }
        if(!is_kept && !there) { t->bare++; free(at_out); return true; }
    }
    if(replace && rp_fs_remove(at_out,&fault)==RP_FS_FAULT) {
        fprintf(stderr,"replay: %s; the run stops here\n",rp_display(fault.why));
        free(at_out);
        return false;
    }
    fflush(stdout); fflush(stderr);
    pid_t pid=fork();
    if(pid<0) { fprintf(stderr,"replay: no process for %s; the run stops here\n",rp_display(name)); free(at_out); return false; }
    if(pid==0) {
        if(rp_crash_page!=NULL && !strcmp(name,rp_crash_page)) abort();
        if(rp_hang_page!=NULL && !strcmp(name,rp_hang_page)) {
            /* Never ending, but not for ever: a parent that does not kill
             * it sees it die of the alarm instead. */
            alarm(20);
            for(;;) pause();
        }
        if(rp_odd_exit_page!=NULL && !strcmp(name,rp_odd_exit_page)) _exit(rp_odd_exit_code);
        if(rp_orphan_page!=NULL && !strcmp(name,rp_orphan_page)) {
            /* What a child killed inside its write leaves: the temporary
             * the door was writing, by its own name. */
            char temp[4096]; snprintf(temp,sizeof(temp),"%s.%ld.new",at_out,(long)getpid());
            (void)rp_fs_write(temp,"address: half\n",14,&fault);
            abort();
        }
        int code=rp_replay_page(kept,out,name,budget_ms);
        if(rp_dirty_page!=NULL && !strcmp(name,rp_dirty_page)) {
            /* What a page could leave behind: the console's buffer full,
             * and the allocator set to fail at its next ask. */
            output_len=sizeof(output); fail_at=attempts+1;
        }
        /* A status naming a state the record does not say. */
        if(rp_lie_page!=NULL && !strcmp(name,rp_lie_page)) code=RP_CHILD_BASE+RP_UNFINISHED;
        if(rp_lie_clean_page!=NULL && !strcmp(name,rp_lie_clean_page)) code^=RP_CHILD_CLEAN;
        fflush(stdout); fflush(stderr);
        _exit(code);
    }
    int status=0; bool timed_out=false;
    bool waited=rp_wait(pid,&status,&timed_out);
    int wait_errno=errno;
    /* The temporary a child that died inside its write leaves. */
    {
        char temp[4096]; snprintf(temp,sizeof(temp),"%s.%ld.new",at_out,(long)pid);
        if(rp_fs_remove(temp,&fault)==RP_FS_FAULT) {
            fprintf(stderr,"replay: %s; the run stops here\n",rp_display(fault.why));
            free(at_out);
            return false;
        }
    }
    int code=waited && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    char why[160]="";
    if(!waited) {
        snprintf(why,sizeof(why),"the replay died: its process could not be waited for (%s)",strerror(wait_errno));
    } else if(timed_out) {
        snprintf(why,sizeof(why),"the replay died: it timed out after %llu s, and was killed",
                 (unsigned long long)(rp_child_limit_ms/1000));
    } else if(code>=RP_CHILD_BASE && code<=RP_CHILD_TOP) {
        RpState st=(RpState)((code-RP_CHILD_BASE)&3);
        bool defaulted=((code-RP_CHILD_BASE)&RP_CHILD_DEFAULTED)!=0;
        bool clean=((code-RP_CHILD_BASE)&RP_CHILD_CLEAN)!=0;
        /* The page's state is read from its record, which must say what
         * the status says: its second line the state, its `REPLAY inputs
         * defaulted:` line there when the status says so, a clean page's
         * record no MISSING or FAILED line of a kind a replay reproduces
         * and anyone else's at least one, and its `end`. Whether it let go
         * of what it held, the record does not say: that flag can only
         * make the run's answer worse. */
        char *text=NULL; size_t size=0;
        bool says=false;
        if(rp_fs_read(at_out,&text,&size,&fault)==RP_FS_OK) {
            char head[64]; snprintf(head,sizeof(head),"\nREPLAY state: %s",kStates[st]);
            const char *line=strstr(text,head);
            unsigned reproduced=0;
            for(size_t at=0; at<size; ) {
                const char *l=text+at, *e=memchr(l,'\n',size-at);
                size_t n=e!=NULL ? (size_t)(e-l) : size-at;
                RpLine k=rp_classify(l,n,false);
                if((!strcmp(k.kind,"missing") || !strcmp(k.kind,"failed")) && rp_reproduced(k.a,k.alen)) reproduced++;
                at+=n+1;
            }
            says=line!=NULL && memchr(text,'\n',(size_t)(line-text))==NULL &&
                 (line[strlen(head)]=='\n' || line[strlen(head)]==' ') &&
                 (strstr(text,"\nREPLAY inputs defaulted: ")!=NULL)==defaulted &&
                 clean==(st==RP_VERDICT && reproduced==0) &&
                 size>=4 && !strcmp(text+size-4,YONDER_DIAG_END "\n");
        }
        free(text);
        if(says) {
            t->states[st]++;
            t->clean+=clean;
            t->leaked+=((code-RP_CHILD_BASE)&RP_CHILD_LEAKED)!=0;
            t->defaulted+=defaulted;
            free(at_out);
            return true;
        }
        snprintf(why,sizeof(why),"the replay died: its record does not say what its process ended with (%s%s%s)",
                 kStates[st],clean ? ", clean" : "",defaulted ? ", its inputs defaulted" : "");
    } else if(WIFSIGNALED(status)) {
        snprintf(why,sizeof(why),"the replay died: signal %d (%s)",WTERMSIG(status),strsignal(WTERMSIG(status)));
    } else {
        snprintf(why,sizeof(why),"the replay died: exit status %d",code);
    }
    /* The parent's record of a page whose replay told it nothing it can
     * trust: its address, if its kept record can be read, its state, and
     * `end`. */
    RpOutcome o; memset(&o,0,sizeof(o));
    o.state=RP_UNJUDGED; snprintf(o.why,sizeof(o.why),"%s",why);
    char *stem=strndup(name,strlen(name)-4), *kept_txt=rp_cat(kept,"/",stem), *kpath=rp_cat(kept_txt,".txt","");
    char *ktext=NULL; size_t ksize=0; RpRecord r; memset(&r,0,sizeof(r));
    const char *address=NULL;
    if(rp_fs_read(kpath,&ktext,&ksize,&fault)==RP_FS_OK && rp_record_read(ktext,ksize,&r)==NULL) address=r.address;
    rp_said.n=0;
    RpText whole=rp_compose(&o,address,NULL,0);
    (void)rp_fs_write(at_out,whole.p,whole.n,&fault);
    free(whole.p); free(ktext); rp_record_free(&r); free(kpath); free(kept_txt); free(stem); free(at_out);
    rp_report(name,&o,false,t);
    return true;
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
    if(got==RP_FS_FAULT) { fprintf(stderr,"replay: %s; nothing was replayed\n",rp_display(fault.why)); return 2; }
    bool same=false;
    if(got==RP_FS_OK && (got=rp_fs_same(kept,out,&same,&fault))==RP_FS_FAULT) {
        fprintf(stderr,"replay: %s; nothing was replayed\n",rp_display(fault.why)); return 2;
    }
    if(same) {
        fprintf(stderr,"replay: %s is the kept directory itself; a replay is written somewhere else\n",rp_display(out));
        return 2;
    }
    if(only==NULL && before!=0) {
        fprintf(stderr,"replay: %s already holds %zu records; a whole replay is written into an empty "
                       "or new directory\n",rp_display(out),before);
        return 2;
    }
    got=rp_fs_list(kept,".txt",&names,&n,&fault);
    if(got!=RP_FS_OK) {
        fprintf(stderr,"replay: %s\n",got==RP_FS_FAULT ? rp_display(fault.why) : "the kept directory is not there");
        return 2;
    }
    if(rp_fs_mkdir(out,&fault)!=RP_FS_OK) {
        rp_fs_names_free(names,n);
        fprintf(stderr,"replay: %s; nothing was replayed\n",rp_display(fault.why));
        return 2;
    }
    for(size_t i=0;i<n;i++)
        if(only==NULL || strstr(names[i],only)!=NULL) {
            if(!rp_one_process(kept,out,names[i],budget_ms,only!=NULL,t)) { rp_fs_names_free(names,n); return 2; }
        }
    rp_fs_names_free(names,n);
    unsigned replayed=t->states[RP_VERDICT]+t->states[RP_INCOMPLETE]+t->states[RP_UNFINISHED];
    printf("replayed %u, clean %u, incomplete %u, unfinished %u",replayed,t->clean,t->states[RP_INCOMPLETE],
           t->states[RP_UNFINISHED]);
    if(t->defaulted) printf("; %u of version 1, their inputs defaulted",t->defaulted);
    if(t->bare) printf("; %u records are not kept pages",t->bare);
    if(t->states[RP_UNJUDGED]) printf("; %u unjudged",t->states[RP_UNJUDGED]);
    if(t->leaked) printf("; %u did not let go of what they held",t->leaked);
    if(only!=NULL)
        printf("; a partial run (--only %s): %s held %zu records before it, which stand except those it "
               "wrote again",rp_display(only),rp_display(out),before);
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
/* A kept page's record written here, of `version` 2 or 1, saying what
 * yonder's says of its kept files (as many bytes as `page` and `lines`
 * index lines) and of the inputs it was read with (the harness's:
 * rp_harness_inputs, the window's settings at their defaults, its zone
 * `330m`), and ending as yonder's does. Version 1 says neither the four
 * inputs version 2 added nor `end`. */
static bool rp_put_record_of(const char *path, const char *record, const char *page, unsigned lines, int version) {
    size_t n=strlen(record)+512;
    char *text=malloc(n), inputs[256]="";
    RpLoad harness; rp_harness_inputs(&harness);
    if(version>=2)
        snprintf(inputs,sizeof(inputs),"agent: %s\ncookies: %s\nclock: %llu\ntz: 330m\n",
                 harness.agent,harness.cookies,(unsigned long long)harness.clock_ms);
    snprintf(text,n,"%s%sbytes: %zu\nkept lines: %u\nmodern: on\nlaid out at: 800x600 px\nzoom: 1000\n"
             "appearance: light\nmotion: full\nanimate: on\nscripts: on\nscript seconds: %d\n%s",
             record,inputs,strlen(page),lines,YONDER_SCRIPT_SECONDS_DEFAULT,version>=2 ? YONDER_DIAG_END "\n" : "");
    bool ok=rp_put(path,text,strlen(text));
    free(text);
    return ok;
}
static bool rp_put_record(const char *path, const char *record, const char *page, unsigned lines) {
    return rp_put_record_of(path,record,page,lines,2);
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
static RpKnown rp_site_lookup(void *ctx, uint32_t kind, const char *url, uint32_t id, RpAnswer *out) {
    (void)kind; (void)id;
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

/* Whether a line of `record` begins with `start`. */
static bool rp_has_line_start(const char *record, const char *start) {
    size_t n=strlen(start);
    for(const char *at=record; at!=NULL && *at!='\0'; at=strchr(at,'\n'), at=at!=NULL ? at+1 : NULL)
        if(strncmp(at,start,n)==0) return true;
    return false;
}

/* A hand-made page: what it proves, where it is from, its bytes and head,
 * the words of its own checks, which its record must not hold, words its
 * record must hold, which say it was read as it is meant to be (a list
 * that begins with a newline is of LINES, one a line, each the start of a
 * record line it must or must not have), the width
 * of the view it is read in (0 for the harness's own), whether it is read
 * with scripts off or the modern switch off, and whether it is read with
 * every other input unlike the harness's (rp_other_inputs). */
typedef struct {
    const char *what;
    const char *address;
    const char *page;
    way_head_t head;
    const char *forbid, *require;   /* space-separated, or lines; NULL for none */
    int32_t width;
    bool scripts_off;
    bool modern_off;                /* read with the modern switch off */
    bool other;                     /* read with rp_other_inputs */
} RpCase;
/* INPUTS UNLIKE THE HARNESS'S, every one a replay takes from the record
 * (REPLAY.md § 3), so a replay that took the harness's value for any of
 * them instead of the record's reads another page: the agent, the cookie
 * string, the clock and the zone, the view's width and height, the zoom,
 * dark pages, reduced motion and animations off. */
static void rp_other_inputs(RpLoad *load) {
    load->agent="other-agent/2"; load->cookies="kept=yes"; load->clock_ms=UINT64_C(1600000000123); load->tz="-120m";
    load->width=900; load->height=700; load->zoom=1250; load->dark=true; load->reduce=true; load->still=true;
}

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
 * and dispatches no input either way, so here the record matches in its
 * picture and font lines too, which a corpus kept on the glass need not,
 * and neither side holds a line an input made. Let be: a `paint stages`
 * line, by its key, whose times are the processor's counter
 * (YONDER_DIAGNOSTICS.md § What paints cost), which no clock of the
 * harness moves; the `kept:`, `kept lines:` and `claim:` facts, which say
 * how the record was made and not what the page did; each record's `end`;
 * and the replay's own lines, its state, its budget and what it does not
 * reproduce. */
static bool rp_let_be(const char *line, size_t len, bool replay) {
    return (len>=6 && !strncmp(line,"kept: ",6)) || (len>=12 && !strncmp(line,"kept lines: ",12)) ||
           (len>=7 && !strncmp(line,"claim: ",7)) ||
           (len==3 && !strncmp(line,"end",3)) ||
           (replay && ((len>=14 && !strncmp(line,"REPLAY state: ",14)) ||
                       (len>=15 && !strncmp(line,"REPLAY budget: ",15)) ||
                       (len>=23 && !strncmp(line,"REPLAY not reproduced: ",23))));
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
typedef enum { RP_COPY, RP_DROP_SCRIPT_LINE, RP_BAD_NUMBER, RP_NO_MEMORY, RP_FULL_TABLE, RP_NO_INDEX, RP_CUT,
               RP_DROP_FACT, RP_SHORT_LINE, RP_CUT_BEFORE_KEPT, RP_BIG_COUNT, RP_ODD_LINE, RP_ESCAPES,
               RP_SWAP_NUMBERS, RP_BIG_LINES, RP_JUNK_BYTES, RP_V1, RP_V3, RP_EMPTY_INDEX, RP_NO_VERSION,
               RP_CR, RP_INDEX_CONTROL, RP_SECOND_ADDRESS, RP_LEADING_ZERO, RP_NOT_UTF8, RP_PAST_END,
               RP_SECOND_VERDICT, RP_PLACEHOLDER, RP_FEWER_LINES, RP_ESCAPED_NOT_UTF8, RP_SECOND_KEPT,
               RP_SECOND_VERSION } RpEdit;
/* The fact RP_DROP_FACT takes out of the record. */
static const char *rp_drop_fact;

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
        /* The script's line taken out leaves the sheets' renumbered, as
         * yonder would have numbered them had the script never been kept. */
        if(edit==RP_DROP_SCRIPT_LINE && !strcmp(kFiles[f],".d/1.js")) continue;
        const char *dest=edit!=RP_DROP_SCRIPT_LINE ? kFiles[f] : !strcmp(kFiles[f],".d/2.css") ? ".d/1.css" :
                         !strcmp(kFiles[f],".d/3.css") ? ".d/2.css" : kFiles[f];
        char *a=rp_cat(from,kFiles[f],""), *b=rp_cat(to,dest,"");
        size_t len=0; char *text=rp_file(a,&len);
        if(text==NULL) { printf("  %s: %s%s was not kept\n",dir,stem,kFiles[f]); bad++; }
        else {
            if(!strcmp(kFiles[f],".d/index") && edit==RP_DROP_SCRIPT_LINE) {
                char *line=strstr(text,"\tscript\thttps://fixture.test/m.js\t");
                char *start=line; while(start!=NULL && start>text && start[-1]!='\n') start--;
                char *end=line!=NULL ? strchr(line,'\n') : NULL;
                if(end==NULL) { printf("  %s: the index has no line for m.js\n",dir); bad++; }
                else { memmove(start,end+1,strlen(end+1)+1); len=strlen(text); }
                /* Renumbered 1 to N. */
                RpText again={0}; unsigned number=0;
                for(char *l=text; *l; ) {
                    char *e=strchr(l,'\n'); size_t n=e!=NULL ? (size_t)(e-l)+1 : strlen(l);
                    if(isdigit((unsigned char)*l)) {
                        char lead[16]; snprintf(lead,sizeof(lead),"%u",++number);
                        size_t d=strspn(l,"0123456789");
                        rp_add(&again,lead,strlen(lead)); rp_add(&again,l+d,n-d);
                    } else rp_add(&again,l,n);
                    l+=n;
                }
                free(text); text=again.p; len=again.n;
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
                /* Before its `end`, where yonder writes them. */
                if(len>=4 && !strcmp(text+len-4,YONDER_DIAG_END "\n")) len-=4;
                text=realloc(text,len+strlen(add)+5); memcpy(text+len,add,strlen(add)); len+=strlen(add);
                memcpy(text+len,YONDER_DIAG_END "\n",5); len+=4;
            }
            if(!strcmp(kFiles[f],".txt") && edit==RP_CUT) {
                /* Cut short just before its `end`, as a quota would cut it,
                 * so only the `end` rule can catch it. */
                if(len<4 || strcmp(text+len-4,YONDER_DIAG_END "\n")) { printf("  %s: the record has no end\n",dir); bad++; }
                else { len-=4; text[len]='\0'; }
            }
            if(!strcmp(kFiles[f],".txt") && edit==RP_CUT_BEFORE_KEPT) {
                /* Cut after its claim's `kept lines:` and before its bytes'
                 * `kept:`, so it says neither `kept:` nor `end`. */
                char *at=strstr(text,"\nkept: whole\n");
                if(at==NULL || strstr(text,"\nkept lines: ")==NULL || strstr(text,"\nkept lines: ")>at) {
                    printf("  %s: the record does not say `kept lines:` before `kept:`\n",dir); bad++;
                } else { len=(size_t)(at+1-text); text[len]='\0'; }
            }
            if(!strcmp(kFiles[f],".txt") && edit==RP_BIG_COUNT) {
                /* A record line's count of twenty digits. */
                char *at=strstr(text,"\nMISSING ");
                char *eol=at!=NULL ? strchr(at+1,'\n') : NULL;
                char *paren=NULL;
                for(char *c=at; eol!=NULL && c<eol; c++) if(*c=='(') paren=c;
                if(paren==NULL) { printf("  %s: the record has no MISSING line\n",dir); bad++; }
                else {
                    static const char kBig[]="(99999999999999999999)";
                    size_t head=(size_t)(paren-text), tail=strlen(eol);
                    char *again=malloc(head+sizeof(kBig)+tail);
                    memcpy(again,text,head); memcpy(again+head,kBig,sizeof(kBig)-1); memcpy(again+head+sizeof(kBig)-1,eol,tail+1);
                    free(text); text=again; len=strlen(text);
                }
            }
            if(!strcmp(kFiles[f],".txt") && edit==RP_ESCAPES) {
                /* A fact the runner quotes, holding a terminal's title
                 * sequence. */
                char *at=strstr(text,"\nkept: whole\n");
                if(at==NULL) { printf("  %s: the record has no kept line\n",dir); bad++; }
                else {
                    static const char kOwned[]="\nkept: not whole: \\x1B]0;owned\\x07\n";
                    size_t head=(size_t)(at-text); const char *rest=at+strlen("\nkept: whole\n");
                    char *again=malloc(head+sizeof(kOwned)+strlen(rest));
                    memcpy(again,text,head); strcpy(again+head,kOwned); strcat(again,rest);
                    free(text); text=again; len=strlen(text);
                }
            }
            /* A fact taken out, or its value spoiled: the line `key: ...`
             * whose key is `drop`, removed, or given `value`. */
            const char *drop=edit==RP_DROP_FACT ? rp_drop_fact : edit==RP_BIG_LINES || edit==RP_FEWER_LINES ? "kept lines" :
                             edit==RP_ESCAPED_NOT_UTF8 ? "agent" :
                             edit==RP_JUNK_BYTES || edit==RP_LEADING_ZERO ? "bytes" : NULL;
            if(!strcmp(kFiles[f],".txt") && drop!=NULL) {
                char key[32]; snprintf(key,sizeof(key),"\n%s: ",drop);
                char *at=strstr(text,key), *eol=at!=NULL ? strchr(at+1,'\n') : NULL;
                if(eol==NULL) { printf("  %s: the record has no `%s`\n",dir,drop); bad++; }
                else {
                    char value[64]="";
                    if(edit==RP_BIG_LINES) snprintf(value,sizeof(value),"\nkept lines: 4294967296");
                    if(edit==RP_ESCAPED_NOT_UTF8) snprintf(value,sizeof(value),"\nagent: \\xFF");
                    if(edit==RP_FEWER_LINES) snprintf(value,sizeof(value),"\nkept lines: %lu",strtoul(at+13,NULL,10)-1);
                    if(edit==RP_JUNK_BYTES) snprintf(value,sizeof(value),"%.*sx",(int)(eol-at),at);
                    if(edit==RP_LEADING_ZERO) snprintf(value,sizeof(value),"\nbytes: 0%.*s",(int)(eol-at-8),at+8);
                    size_t head=(size_t)(at-text), tail=strlen(eol);
                    char *again=malloc(head+strlen(value)+tail+1);
                    memcpy(again,text,head); strcpy(again+head,value); strcat(again,eol);
                    free(text); text=again; len=strlen(text);
                }
            }
            if(!strcmp(kFiles[f],".txt") && (edit==RP_NOT_UTF8 || edit==RP_PAST_END || edit==RP_SECOND_VERDICT ||
                                              edit==RP_SECOND_KEPT)) {
                /* A plain fact whose bytes are not UTF-8, a second
                 * verdict, or a second `kept:` after the record's `kept:
                 * whole`, before its `end`; or a line after it. */
                const char *add=edit==RP_NOT_UTF8 ? "note: caf\xE9\n" : edit==RP_SECOND_VERDICT ? "verdict: clean\n" :
                                edit==RP_SECOND_KEPT ? "kept: not whole: a step of keeping the page failed\n"
                                                     : "note: after\n";
                if(len<4 || strcmp(text+len-4,YONDER_DIAG_END "\n")) { printf("  %s: the record has no end\n",dir); bad++; }
                else {
                    text=realloc(text,len+strlen(add)+1);
                    size_t at=edit==RP_PAST_END ? len : len-4;
                    memmove(text+at+strlen(add),text+at,len-at+1);
                    memcpy(text+at,add,strlen(add)); len+=strlen(add);
                }
            }
            if(!strcmp(kFiles[f],".txt") && edit==RP_PLACEHOLDER) {
                /* The claim's placeholder, never replaced, beside the kept
                 * files: a name claimed and a record never written. */
                char *nl=strchr(text,'\n');
                if(nl==NULL) { printf("  %s: the record has no address\n",dir); bad++; }
                else { nl[1]='\0'; text=realloc(text,strlen(text)+32); strcat(text,"verdict: unwritten\n"); len=strlen(text); }
            }
            if(!strcmp(kFiles[f],".d/index") && edit==RP_SWAP_NUMBERS) {
                /* The two sheets' lines numbered each as the other: each
                 * names a file that is there, and the count is right, so
                 * only the numbering rule can tell. */
                char *two=strstr(text,"\n2\tsheet\t"), *three=strstr(text,"\n3\tsheet\t");
                if(two==NULL || three==NULL) { printf("  %s: the index has no sheets numbered 2 and 3\n",dir); bad++; }
                else { two[1]='3'; three[1]='2'; }
            }
            if(!strcmp(kFiles[f],".d/index") && (edit==RP_V1 || edit==RP_V3)) {
                /* Version 1: each line without its job's identity, as
                 * yonder wrote it before version 2; version 3: one no
                 * runner reads. */
                char *v=strstr(text,"index 2\n");
                if(v==NULL) { printf("  %s: the index is not version 2\n",dir); bad++; }
                else v[6]=edit==RP_V1 ? '1' : '3';
                RpText again={0};
                for(char *l=text; edit==RP_V1 && *l; ) {
                    char *e=strchr(l,'\n'); size_t n=e!=NULL ? (size_t)(e-l) : strlen(l), keep=n;
                    if(*l>='1' && *l<='9') { while(keep>0 && l[keep-1]!='\t') keep--; if(keep>0) keep--; }
                    rp_add(&again,l,keep);
                    if(e!=NULL) rp_add(&again,"\n",1);
                    l+=n+(e!=NULL);
                }
                if(edit==RP_V1) { free(text); text=again.p; len=again.n; }
            }
            if(!strcmp(kFiles[f],".d/index") && edit==RP_EMPTY_INDEX) { text[0]='\0'; len=0; }
            if(!strcmp(kFiles[f],".d/index") && edit==RP_NO_VERSION) {
                /* Its version line made a comment: the lines are whole, and
                 * nothing says which version's they are. */
                if(strncmp(text,"# yonder kept index ",20)) { printf("  %s: the index has no version line\n",dir); bad++; }
                else text[2]='Y';
            }
            if(!strcmp(kFiles[f],".d/index") && edit==RP_INDEX_CONTROL) {
                /* A raw control byte in a field of a line otherwise whole. */
                char *at=strstr(text,"https://cdn.fixture.test/i.css\tcame");
                if(at==NULL) { printf("  %s: the index has no line for i.css\n",dir); bad++; }
                else at[8]='\x01';
            }
            if(!strcmp(kFiles[f],".txt") && (edit==RP_CR || edit==RP_SECOND_ADDRESS)) {
                /* A carriage return inside a plain fact; or a second
                 * `address:` among the facts. */
                char *at=strstr(text,"\nwritten: ");
                if(at==NULL) { printf("  %s: the record has no `written:`\n",dir); bad++; }
                else if(edit==RP_CR) at[5]='\r';
                else memcpy(at+1,"address: ",9);
            }
            if(!strcmp(kFiles[f],".d/index") && edit==RP_ODD_LINE) {
                /* A line whose kind is cut short: damage, not a kind
                 * unknown. */
                static const char kOdd[]="9\tscr\n";
                text=realloc(text,len+sizeof(kOdd)); memcpy(text+len,kOdd,sizeof(kOdd)); len+=sizeof(kOdd)-1;
            }
            if(!strcmp(kFiles[f],".d/index") && edit==RP_SECOND_VERSION) {
                /* The version line again, after the lines: whole, and of
                 * the index's own version, so only its standing once can
                 * tell. */
                char again[48];
                int n=snprintf(again,sizeof(again),"# yonder kept index %d\n",YONDER_DIAG_INDEX_VERSION);
                text=realloc(text,len+(size_t)n+1); memcpy(text+len,again,(size_t)n+1); len+=(size_t)n;
            }
            if(!strcmp(kFiles[f],".d/index") && edit==RP_SHORT_LINE) {
                static const char kShort[]="9\tscript\thttps://fixture.test/x.js\tcame\n";
                text=realloc(text,len+sizeof(kShort)); memcpy(text+len,kShort,sizeof(kShort)); len+=sizeof(kShort)-1;
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

static unsigned rp_corpus_cases(const char *work, const char *kept, const char *name, const char *other) {
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

    /* THE REFUSALS, each by the one rule that can catch it, so its record
     * must say THAT rule's reason (a case another rule catches first, or a
     * child that died on the way, proves nothing): an index line whose
     * number is not one, a record that lost a line (with no memory to
     * write it, or no room: the line past the cap may be the one a change
     * made), a `*.txt` that is not a record, a record cut just before its
     * `end`, one cut after its claim's `kept lines:` and before its
     * `kept:` (it is a kept page's, not a page never kept), one with a
     * count of twenty digits, one whose quoted fact holds a terminal
     * sequence (the .sh checks the runner prints it re-spelled), index
     * lines short of their fields or of a kind cut short, two sheets'
     * lines numbered each as the other, a `kept lines` past UINT32_MAX (it
     * would wrap to 0), a `bytes` with something after its digits or a
     * zero before them, an index of version 1 beside a record of version 2
     * and one of a version no runner reads, an index that is empty or
     * whose first line is not its version, a carriage return in a fact, a
     * control byte in an index field, a second `address:`, a `kept: not
     * whole` after the record's `kept: whole` (a second head fact), the
     * index's version line again after its lines, a plain fact
     * whose bytes are not UTF-8, a line past `end`, a second verdict, the
     * claim's placeholder never replaced, a `kept lines` one short of its
     * index, and an agent escaped to a byte that is no part of UTF-8: none
     * is judged, each says why on a record of its own, and
     * each makes the answer 2. */
    static const struct { const char *dir; RpEdit edit; const char *why; } kRefused[]={
        {"/badnumber",RP_BAD_NUMBER,"its index is damaged: a count no record holds"},
        {"/nomemory",RP_NO_MEMORY,"its record lost lines it could not keep"},
        {"/fulltable",RP_FULL_TABLE,"its record lost lines it could not keep"},
        {"/notarecord",RP_COPY,"it is not a page file: its first line is not `address: `"},
        {"/cut",RP_CUT,"its record has no `end`: it was cut short"},
        {"/shortline",RP_SHORT_LINE,"its index is damaged: a line that is not a script's or a sheet's whole"},
        {"/cutbeforekept",RP_CUT_BEFORE_KEPT,"its record has no `end`: it was cut short"},
        {"/bigcount",RP_BIG_COUNT,"it is not a whole page file: a count no record holds"},
        {"/oddline",RP_ODD_LINE,"its index is damaged: a line that is not a script's or a sheet's whole"},
        {"/escapes",RP_ESCAPES,"it was kept not whole: "},
        {"/swapnumbers",RP_SWAP_NUMBERS,"its index is damaged: its lines are not numbered 1 to N in order, each once"},
        {"/biglines",RP_BIG_LINES,"it is not a whole page file: a count no record holds"},
        {"/junkbytes",RP_JUNK_BYTES,"it is not a whole page file: a count no record holds"},
        {"/leadingzero",RP_LEADING_ZERO,"it is not a whole page file: a count no record holds"},
        {"/version1",RP_V1,"its record is of version 2 and its index of version 1"},
        {"/version3",RP_V3,"its index is of version 3, which this runner does not read"},
        {"/emptyindex",RP_EMPTY_INDEX,"its index is empty"},
        {"/noversion",RP_NO_VERSION,"its index is damaged: its first line is not its version"},
        {"/carriagereturn",RP_CR,"it is not a whole page file: a control byte"},
        {"/indexcontrol",RP_INDEX_CONTROL,"its index is damaged: a control byte"},
        {"/secondaddress",RP_SECOND_ADDRESS,"it is not a whole page file: a second `address:`"},
        {"/secondkept",RP_SECOND_KEPT,"it is not a whole page file: a second `kept:`"},
        {"/secondversion",RP_SECOND_VERSION,"its index is damaged: a second version line"},
        {"/notutf8",RP_NOT_UTF8,"it is not a page file: it is not UTF-8"},
        {"/pastend",RP_PAST_END,"it is not a whole page file: lines past its `end`"},
        {"/secondverdict",RP_SECOND_VERDICT,"it is not a whole page file: a second verdict line"},
        {"/placeholder",RP_PLACEHOLDER,"its name was claimed and its record never written"},
        {"/fewerlines",RP_FEWER_LINES,"its record says its index holds 2 lines, and it holds 3"},
        {"/escapednotutf8",RP_ESCAPED_NOT_UTF8,"it is not a whole page file: a field that escapes to bytes that are not UTF-8"}};
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
        char state[512]; snprintf(state,sizeof(state),"\nREPLAY state: unjudged %s",kRefused[i].why);
        if(rc!=2 || t.states[RP_UNJUDGED]!=1 || rp_replayed(&t)!=judged || unjudged==NULL ||
           strstr(unjudged,state)==NULL) {
            printf("  %s: a corpus with a page nobody can judge answered %d, its record not saying `%s`\n",
                   kRefused[i].dir,rc,kRefused[i].why);
            bad++;
        }
        free(unjudged); free(said); free(record); free(dir);
    }

    /* EVERY INPUT A RECORD OF VERSION 2 MUST SAY (REPLAY.md § 3), each
     * taken out of a copy of a network page's record in turn: the page is
     * unjudged, its record saying which. Listed here, not drawn from the
     * runner's own table, so a fact the runner stops requiring is a case
     * that fails. */
    static const char *const kInputs[]={"final address","body","kept lines","laid out at","zoom","appearance",
        "motion","animate","modern","scripts","script seconds","agent","cookies","clock","tz","status",
        "content-type","charset","encrypted"};
    for(size_t i=0;i<sizeof(kInputs)/sizeof(kInputs[0]);i++) {
        char sub[64]; snprintf(sub,sizeof(sub),"/no-%s",kInputs[i]);
        for(char *c=sub; *c; c++) if(*c==' ') *c='-';
        dir=rp_cat(work,sub,"");
        rp_drop_fact=kInputs[i];
        bad+=rp_copy_page(kept,name,dir,RP_DROP_FACT);
        rp_drop_fact=NULL;
        rc=rp_replay_copy(dir,name,60000,&t,&record);
        char state[160]; snprintf(state,sizeof(state),"\nREPLAY state: unjudged its record does not say its `%s`\n",
                                  kInputs[i]);
        if(rc!=2 || t.states[RP_UNJUDGED]!=1 || record==NULL || strstr(record,state)==NULL) {
            printf("  a record without its `%s` was not unjudged for it (%d)\n",kInputs[i],rc); bad++;
        }
        free(record); free(dir);
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

    /* A page that asks for one address more often than where it was kept,
     * of version 1 so its index records no identity to tell the two jobs
     * apart: each line answers one job, so the extra job is not answered
     * from the first line again (which would run the script twice); it is
     * incomplete, and, since it blocks the parse, the page is unfinished.
     * Then a whole replay into an OUT-DIR that already holds records is
     * refused, and a partial one adds to it. */
    dir=rp_cat(work,"/twice","");
    char *dfolder=rp_cat(dir,"/fixture.test-0001.d","");
    if(!rp_put_dir(dir) || !rp_put_dir(dfolder)) bad++;
    static const char kTwiceRecord[]="address: http://fixture.test/twice\nverdict: clean\nstatus: 200 OK\n"
        "final address: http://fixture.test/twice\nbody: html\ncontent-type: text/html\ncharset: \nencrypted: no\nkept: whole\n";
    static const char kTwicePage[]="<!doctype html><p>twice</p><script src=t.js></script><script src=t.js></script>";
    static const char kTwiceIndex[]="# yonder kept index 1\n1\tscript\thttp://fixture.test/t.js\tcame\thttp://fixture.test/t.js\t\n";
    char *f1=rp_cat(dir,"/fixture.test-0001.txt",""), *f2=rp_cat(dir,"/fixture.test-0001.html","");
    char *f3=rp_cat(dfolder,"/index",""), *f4=rp_cat(dfolder,"/1.js","");
    if(!rp_put_record_of(f1,kTwiceRecord,kTwicePage,1,1) || !rp_put(f2,kTwicePage,sizeof(kTwicePage)-1) ||
       !rp_put(f3,kTwiceIndex,sizeof(kTwiceIndex)-1) || !rp_put(f4,"1",1)) bad++;
    /* The agent and the zone this version 1 record does not say, stated
     * as the harness's own for the runs of it below. */
    if(rp_window_state("agent=" YONDER_AGENT ",tz=330m")!=NULL) bad++;
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
    rp_window_clear();
    free(again); free(f1); free(f2); free(f3); free(f4); free(dfolder); free(dir);

    /* Two async scripts that came back in the reverse of the order they
     * were asked for: the replay hands them over as they came where the
     * page was kept, and the page sees that order. */
    dir=rp_cat(work,"/order","");
    dfolder=rp_cat(dir,"/fixture.test-0001.d","");
    if(!rp_put_dir(dir) || !rp_put_dir(dfolder)) bad++;
    static const char kOrderRecord[]="address: http://fixture.test/order\nverdict: clean\nstatus: 200 OK\n"
        "final address: http://fixture.test/order\nbody: html\ncontent-type: text/html\ncharset: \nencrypted: no\nkept: whole\n";
    static const char kOrderPage[]="<!doctype html><script>var order=[];</script>"
        "<script async src=a.js></script><script async src=b.js></script>"
        "<script>setTimeout(function(){if(order.join()!='b,a')wrongOrder()},1000)</script>";
    static const char kOrderIndex[]="# yonder kept index 2\n"
        "1\tscript\thttp://fixture.test/b.js\tcame\thttp://fixture.test/b.js\t\t3\n"
        "2\tscript\thttp://fixture.test/a.js\tcame\thttp://fixture.test/a.js\t\t2\n";
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
        "final address: http://fixture.test/goes\nbody: html\ncontent-type: text/html\ncharset: \nencrypted: no\nkept: whole\n";
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
        "final address: http://fixture.test/pic\nbody: html\ncontent-type: text/html\ncharset: \nencrypted: no\nkept: whole\n";
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
    rp_fail_check_at="http://fixture.test/kept";
    rc=rp_replay_copy(dir,name,60000,&t,&record);
    rp_fail_check_at=NULL;
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
        "final address: http://fixture.test/unasked\nbody: html\ncontent-type: text/html\ncharset: \nencrypted: no\nkept: whole\n";
    static const char kUnaskedPage[]="<!doctype html><p>no script now</p>";
    static const char kUnaskedIndex[]="# yonder kept index 2\n"
        "1\tscript\thttp://fixture.test/a%25b\\\\c.js\tcame\thttp://fixture.test/a%25b\\\\c.js\t\t1\n";
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

    /* A record yonder wrote with keep_pages off (of version 2, which is
     * kept only when it says so), beside page bytes of the same name: not
     * a kept page, passed over and never replayed. Under a
     * partial run, a record of it already written is replaced by one that
     * says it is unjudged, so no earlier run's record stands for it. */
    dir=rp_cat(work,"/notkept","");
    char *stale=rp_cat(dir,"-out","");
    if(!rp_put_dir(dir) || !rp_put_dir(stale)) bad++;
    static const char kNotKept[]="address: http://fixture.test/plain\nverdict: clean\nagent: a\ncookies: \n"
        "clock: 1\ntz: 330m\nstatus: 200 OK\nfinal address: http://fixture.test/plain\nbody: html\n"
        "content-type: text/html\nencrypted: no\nend\n";
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

    /* A partial run whose new record cannot be written (a file-size limit
     * every process of the run inherits, past which no record fits)
     * leaves no record, never the old one; one whose old record cannot be
     * removed (a directory with something in it, by that name) refuses the
     * run. */
    char *was=rp_cat(dir,"-out/",name);
    if(rp_put(was,"address: stale\n",15)) {
        memset(&t,0,sizeof(t));
        char *outdir=rp_cat(dir,"-out","");
        struct rlimit limit, small;
        void (*was_xfsz)(int)=signal(SIGXFSZ,SIG_IGN);
        bool limited=getrlimit(RLIMIT_FSIZE,&limit)==0;
        small=limit; small.rlim_cur=16;
        limited=limited && setrlimit(RLIMIT_FSIZE,&small)==0;
        rc=rp_replay_dir(dir,outdir,60000,"fixture",&t);
        if(limited) (void)setrlimit(RLIMIT_FSIZE,&limit);
        signal(SIGXFSZ,was_xfsz);
        record=rp_file(was,NULL);
        if(!limited || rc!=2 || record!=NULL) {
            printf("  a partial run whose record could not be written left the old one standing\n"); bad++;
        }
        free(record);
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

    /* A link planted at the name a write's temporary will take (each
     * process's own, by its number): the door makes its temporary new or
     * not at all, so the write faults, the link is never written through,
     * its target is untouched, and nothing is written in its place. */
    dir=rp_cat(work,"/planted","");
    if(!rp_put_dir(dir)) bad++;
    char *target=rp_cat(work,"/planted-target",""), *written=rp_cat(dir,"/",name);
    if(!rp_put(target,"untouched",9)) bad++;
    {
        char trap[4096]; snprintf(trap,sizeof(trap),"%s.%ld.new",written,(long)getpid());
        RpFault fault;
        if(rp_fs_symlink(target,trap,&fault)!=RP_FS_OK) { printf("  %s\n",fault.why); bad++; }
        bool faulted=rp_fs_write(written,"a record\n",9,&fault)==RP_FS_FAULT;
        record=rp_file(target,NULL);
        char *none=rp_file(written,NULL);
        if(!faulted || record==NULL || strcmp(record,"untouched") || none!=NULL) {
            printf("  a link planted at a temporary's name was written through\n"); bad++;
        }
        free(none); free(record);
    }
    free(written); free(target); free(dir);

    /* ONE PROCESS PER PAGE: a page whose replay dies leaves a record the
     * parent writes, saying so, and the next page replays as it would
     * alone; a page whose replay leaves the fakes dirty (the console's
     * buffer full, the allocator set to fail) leaves the next page as it
     * would be alone too. Two copies of one page, the second under another
     * name, each run once crashing the first and once dirtying it. */
    for(int run=0;run<2;run++) {
        dir=rp_cat(work,run==0 ? "/crashes" : "/dirties","");
        bad+=rp_copy_page(kept,name,dir,RP_COPY);
        char *second=strdup(name); second[strlen(second)-5]='9';
        char *from_stem=strndup(name,strlen(name)-4), *to_stem=strndup(second,strlen(second)-4);
        static const char *const kParts[]={".txt",".html",".d/1.js",".d/2.css",".d/3.css",".d/index"};
        char *d2=rp_cat(dir,"/",to_stem); char *d2d=rp_cat(d2,".d","");
        if(!rp_put_dir(d2d)) bad++;
        for(size_t k=0;k<sizeof(kParts)/sizeof(kParts[0]);k++) {
            char *a=rp_cat(dir,"/",from_stem), *aa=rp_cat(a,kParts[k],""), *bb=rp_cat(d2,kParts[k],"");
            size_t len=0; char *bytes=rp_file(aa,&len);
            if(bytes==NULL || !rp_put(bb,bytes,len)) bad++;
            free(bytes); free(a); free(aa); free(bb);
        }
        if(run==0) rp_crash_page=name; else rp_dirty_page=name;
        char *outdir=rp_cat(dir,"-out","");
        memset(&t,0,sizeof(t));
        rc=rp_replay_dir(dir,outdir,60000,NULL,&t);
        rp_crash_page=rp_dirty_page=NULL;
        char *first_at=rp_cat(outdir,"/",name), *second_at=rp_cat(outdir,"/",second);
        char *one=rp_file(first_at,NULL), *two=rp_file(second_at,NULL);
        bool died_said=one!=NULL && strstr(one,"\nREPLAY state: unjudged the replay died: signal ")!=NULL;
        /* The second page as it replays alone: a verdict, with the
         * verdict line the first page's own replay has. */
        bool second_judged=two!=NULL && strstr(two,"\nREPLAY state: verdict\n")!=NULL;
        const char *v1=one!=NULL ? strstr(one,"\nverdict: ") : NULL, *v2=two!=NULL ? strstr(two,"\nverdict: ") : NULL;
        bool same_verdict=v1!=NULL && v2!=NULL && strcspn(v1+1,"\n")==strcspn(v2+1,"\n") &&
                          !strncmp(v1,v2,strcspn(v1+1,"\n")+1);
        if(run==0 && (rc!=2 || !died_said || !second_judged || t.states[RP_UNJUDGED]!=1)) {
            printf("  a page whose replay died did not leave its record, or reached the next page\n"); bad++;
        }
        if(run==1 && (rc!=0 || !second_judged || !same_verdict || t.states[RP_VERDICT]!=2)) {
            printf("  a page that dirtied the fakes reached the next page\n"); bad++;
        }
        free(one); free(two); free(first_at); free(second_at); free(outdir); free(d2); free(d2d);
        free(from_stem); free(to_stem); free(second); free(dir);
    }

    /* WHAT THE PARENT DOES NOT TRUST (REPLAY.md § 6), a copy of the first
     * hand-made page each: a child whose status names a state its record
     * does not say, and one whose status calls a page clean that its
     * record does not; one killed inside its record's write, whose
     * temporary the parent removes; one that never ends, killed at its
     * time; and two that exit with a status outside the table, 1 as a
     * sanitizer's death does and 97 just past the table's top. Each page
     * is unjudged on a record the parent writes, saying why, and no
     * temporary is left in OUT-DIR. */
    static const struct { const char *dir; const char **hook; int code; const char *why; } kDistrust[]={
        {"/lies",&rp_lie_page,0,"the replay died: its record does not say what its process ended with (unfinished)"},
        {"/liesclean",&rp_lie_clean_page,0,
         "the replay died: its record does not say what its process ended with (verdict, clean)"},
        {"/orphan",&rp_orphan_page,0,"the replay died: signal 6 (Aborted)"},
        {"/hangs",&rp_hang_page,0,"the replay died: it timed out after 2 s, and was killed"},
        {"/oddexit",&rp_odd_exit_page,1,"the replay died: exit status 1"},
        {"/oddexit97",&rp_odd_exit_page,97,"the replay died: exit status 97"}};
    for(size_t i=0;i<sizeof(kDistrust)/sizeof(kDistrust[0]);i++) {
        dir=rp_cat(work,kDistrust[i].dir,"");
        bad+=rp_copy_page(kept,name,dir,RP_COPY);
        /* Two seconds for the one that never ends: the others are let
         * run to their end, however slowly a loaded machine runs them. */
        *kDistrust[i].hook=name; rp_odd_exit_code=kDistrust[i].code;
        if(kDistrust[i].hook==&rp_hang_page) rp_child_limit_ms=2000;
        rc=rp_replay_copy(dir,name,60000,&t,&record);
        *kDistrust[i].hook=NULL; rp_child_limit_ms=RP_CHILD_LIMIT_MS;
        char state[256]; snprintf(state,sizeof(state),"\nREPLAY state: unjudged %s\nend\n",kDistrust[i].why);
        char *outdir=rp_cat(dir,"-out",""), **left=NULL; size_t nleft=0;
        RpFault fault;
        if(rp_fs_list(outdir,".new",&left,&nleft,&fault)!=RP_FS_OK) nleft=1;
        if(rc!=2 || t.states[RP_UNJUDGED]!=1 || record==NULL || strstr(record,state)==NULL || nleft!=0) {
            printf("  %s: the parent trusted what it should not, or left %zu temporaries (%d)\n",kDistrust[i].dir,nleft,rc);
            bad++;
        }
        rp_fs_names_free(left,nleft);
        free(outdir); free(record); free(dir);
    }

    /* TWO JOBS OF ONE ADDRESS, each with an outcome of its own: two
     * sheets of one address (each its own place in the page's table),
     * kept in the reverse of the order the page asks for them. Each takes
     * the line kept for its own identity, so the later sheet's rule wins
     * as it did where the page was kept, and the page is clean. */
    dir=rp_cat(work,"/identity","");
    dfolder=rp_cat(dir,"/fixture.test-0001.d","");
    if(!rp_put_dir(dir) || !rp_put_dir(dfolder)) bad++;
    static const char kIdRecord[]="address: http://fixture.test/two\nverdict: clean\nstatus: 200 OK\n"
        "final address: http://fixture.test/two\nbody: html\ncontent-type: text/html\ncharset: \nencrypted: no\nkept: whole\n";
    static const char kIdPage[]="<!doctype html><link rel=stylesheet href=s.css><link rel=stylesheet href=s.css>"
        "<p id=p>two</p><script>setTimeout(function(){if(document.getElementById('p').offsetWidth!=100)swapped()},1000)"
        "</script>";
    static const char kIdIndex[]="# yonder kept index 2\n"
        "1\tsheet\thttp://fixture.test/s.css\tcame\thttp://fixture.test/s.css\t\t1\n"
        "2\tsheet\thttp://fixture.test/s.css\tcame\thttp://fixture.test/s.css\t\t0\n";
    f1=rp_cat(dir,"/fixture.test-0001.txt",""); f2=rp_cat(dir,"/fixture.test-0001.html","");
    f3=rp_cat(dfolder,"/index",""); f4=rp_cat(dfolder,"/1.css",""); f5=rp_cat(dfolder,"/2.css","");
    if(!rp_put_record(f1,kIdRecord,kIdPage,2) || !rp_put(f2,kIdPage,sizeof(kIdPage)-1) ||
       !rp_put(f3,kIdIndex,sizeof(kIdIndex)-1) || !rp_put(f4,"p{width:100px}",14) || !rp_put(f5,"p{width:300px}",14))
        bad++;
    rc=rp_replay_copy(dir,"fixture.test-0001.txt",60000,&t,&record);
    if(rc!=0 || t.clean!=1 || record==NULL || strstr(record,"swapped")!=NULL) {
        printf("  two sheets of one address did not each take their own kept line\n"); bad++;
    }
    free(record); free(f1); free(f2); free(f3); free(f4); free(f5); free(dfolder); free(dir);

    /* KEPT PAGES OF VERSION 1 (REPLAY.md § 3): a record whole but for the
     * four inputs version 2 added and its `end`, an index of six fields,
     * and `--window` stating the agent and the zone. It is judged, its job
     * matched by kind and address in the kept order, the cookies and the
     * clock the harness's and named, the agent and the zone named with
     * the values stated. Then the same record without its `kept:` (one
     * kept before that fact existed): kept, its bytes beside it matching
     * its `bytes:`, and its record saying the assumption. Then one without
     * its `zoom`, which nobody stated: unjudged, saying so. */
    static const char kOldPage[]="<!doctype html><p>old</p><script src=a.js></script>";
    static const char kOldRecord[]="address: http://fixture.test/old\nverdict: clean\nstatus: 200 OK\n"
        "final address: http://fixture.test/old\nbody: html\ncontent-type: text/html\ncharset: \nencrypted: no\nkept: whole\n";
    static const char kOldIndex[]="# yonder kept index 1\n"
        "1\tscript\thttp://fixture.test/a.js\tcame\thttp://fixture.test/a.js\t\n";
    static const char *const kStrip[]={NULL,"kept: whole\n","zoom: 1000\n"};
    char statement[256], stated_line[256];
    snprintf(statement,sizeof(statement),"agent=%s,tz=330m",YONDER_AGENT);
    {
        RpText said={0}; rp_add_escaped(&said,YONDER_AGENT);
        snprintf(stated_line,sizeof(stated_line),"\nREPLAY inputs stated: agent=");
        for(size_t i=0;i<said.n;i++) {
            size_t at=strlen(stated_line);
            if(said.p[i]==' ') snprintf(stated_line+at,sizeof(stated_line)-at,"\\x20");
            else { stated_line[at]=said.p[i]; stated_line[at+1]='\0'; }
        }
        size_t at=strlen(stated_line);
        snprintf(stated_line+at,sizeof(stated_line)-at," tz=330m\nREPLAY inputs defaulted: cookies clock\n");
        free(said.p);
    }
    for(int variant=0;variant<3;variant++) {
        static const char *const kDirs[]={"/version1page","/version1assumed","/version1unstated"};
        dir=rp_cat(work,kDirs[variant],"");
        dfolder=rp_cat(dir,"/fixture.test-0001.d","");
        if(!rp_put_dir(dir) || !rp_put_dir(dfolder)) bad++;
        f1=rp_cat(dir,"/fixture.test-0001.txt",""); f2=rp_cat(dir,"/fixture.test-0001.html","");
        f3=rp_cat(dfolder,"/index",""); f4=rp_cat(dfolder,"/1.js","");
        if(!rp_put_record_of(f1,kOldRecord,kOldPage,1,1) || !rp_put(f2,kOldPage,sizeof(kOldPage)-1) ||
           !rp_put(f3,kOldIndex,sizeof(kOldIndex)-1) || !rp_put(f4,"document.title='old'",20)) bad++;
        if(kStrip[variant]!=NULL) {
            size_t n=0; char *text=rp_file(f1,&n), *at=text!=NULL ? strstr(text,kStrip[variant]) : NULL;
            if(at==NULL) { printf("  %s: the record has no %s",kDirs[variant],kStrip[variant]); bad++; }
            else { memmove(at,at+strlen(kStrip[variant]),strlen(at+strlen(kStrip[variant]))+1); if(!rp_put(f1,text,strlen(text))) bad++; }
            free(text);
        }
        if(rp_window_state(statement)!=NULL) bad++;
        rc=rp_replay_copy(dir,"fixture.test-0001.txt",60000,&t,&record);
        rp_window_clear();
        bool judged=rc==0 && t.clean==1 && t.defaulted==1 && record!=NULL && strstr(record,stated_line)!=NULL &&
                    (strstr(record,"\n" RP_KEPT_ASSUMED "\n")!=NULL)==(variant==1);
        if(variant<2 && !judged) {
            printf("  %s: a kept page of version 1 was not judged with what it took named (%d)\n",kDirs[variant],rc); bad++;
        }
        if(variant==2 && (rc!=2 || record==NULL ||
           strstr(record,"\nREPLAY state: unjudged its record does not say its `zoom` and --window does not state it\n")==NULL)) {
            printf("  %s: a version 1 page missing an input nobody stated was judged (%d)\n",kDirs[variant],rc); bad++;
        }
        if(variant<2) { free(record); free(f1); free(f2); free(f3); free(f4); free(dfolder); free(dir); }
    }

    /* A VERSION 1 PAGE READ IN THE WINDOW `--window` STATES: the page read
     * with every input unlike the harness's, its record made version 1
     * (none of the four inputs version 2 added, no window, no `end`, and
     * no `charset:`, which the version 1 builds left out when empty), and
     * replayed with the window it was read in stated. Its script writes
     * each stated input into its record as it did where it was kept; the
     * cookies and the clock are the harness's, and named. */
    {
        static const char *const kWindowFacts[]={"agent","cookies","clock","tz","laid out at","scripts","modern","zoom",
                                                 "appearance","motion","animate","script seconds","charset"};
        char *wdir=rp_cat(work,"/version1stated",""), *stem=strndup(other,strlen(other)-4);
        char *from=rp_cat(kept,"/",stem), *to=rp_cat(wdir,"/",stem);
        if(!rp_put_dir(wdir)) bad++;
        for(int part=0;part<2;part++) {
            char *a=rp_cat(from,part ? ".html" : ".txt",""), *b=rp_cat(to,part ? ".html" : ".txt","");
            size_t n=0; char *text=rp_file(a,&n);
            if(text==NULL) { printf("  the page read with other inputs was not kept\n"); bad++; }
            else {
                if(!part) {
                    RpText v1={0};
                    for(char *l=text; *l; ) {
                        char *e=strchr(l,'\n'); size_t len=e!=NULL ? (size_t)(e-l)+1 : strlen(l);
                        bool drop=!strncmp(l,"end\n",4) && len==4;
                        for(size_t k=0;!drop && k<sizeof(kWindowFacts)/sizeof(kWindowFacts[0]);k++) {
                            size_t kl=strlen(kWindowFacts[k]);
                            drop=!strncmp(l,kWindowFacts[k],kl) && l[kl]==':';
                        }
                        if(!drop) rp_add(&v1,l,len);
                        l+=len;
                    }
                    free(text); text=v1.p; n=v1.n;
                }
                if(!rp_put(b,text,n)) bad++;
            }
            free(text); free(a); free(b);
        }
        if(rp_window_state("zoom=1250,appearance=dark,motion=reduce,animate=off,modern=on,scripts=on,"
                           "script-seconds=5,laid-out-at=900x700,agent=other-agent/2,tz=-120m")!=NULL) bad++;
        char *wrecord=NULL;
        rc=rp_replay_copy(wdir,other,60000,&t,&wrecord);
        rp_window_clear();
        static const char *const kSaid[]={"agent_other_agent_2","zone_120","dark_111","motion_122","wide_720",
                                          "high_280","animate_704"};
        bool all=rc==0 && wrecord!=NULL && t.defaulted==1 &&
                 strstr(wrecord,"\nREPLAY inputs stated: zoom=1250 appearance=dark motion=reduce animate=off modern=on "
                               "scripts=on script-seconds=5 laid-out-at=900x700\\x20px agent=other-agent/2 tz=-120m\n")!=NULL &&
                 strstr(wrecord,"\nREPLAY inputs defaulted: cookies clock\n")!=NULL;
        for(size_t k=0;all && k<sizeof(kSaid)/sizeof(kSaid[0]);k++) {
            char line[96]; snprintf(line,sizeof(line),"\nMISSING global %s (1)\n",kSaid[k]);
            all=strstr(wrecord,line)!=NULL;
        }
        if(!all) { printf("  a version 1 page was not read in the window --window states (%d)\n",rc); bad++; }
        free(wrecord); free(from); free(to); free(stem); free(wdir);
    }
    free(record); free(f1); free(f2); free(f3); free(f4); free(dfolder); free(dir);
    return bad;
}

/* THE GRAMMAR'S VECTORS (tools/yonder_record_grammar.txt), each a kind and
 * a line, run through rp_classify, which must answer every one's kind:
 * the same file tools/test_yonder_records.py runs through the Python
 * reader, so the two readers agree line for line. The problems found. */
static unsigned rp_grammar_vectors(void) {
    char *text=NULL; size_t size=0; unsigned bad=0, n=0;
    RpFault fault;
    if(rp_fs_read("tools/yonder_record_grammar.txt",&text,&size,&fault)!=RP_FS_OK) {
        printf("  the grammar's vectors cannot be read\n"); return 1;
    }
    for(size_t at=0; at<size; ) {
        const char *line=text+at, *end=memchr(line,'\n',size-at);
        size_t len=end!=NULL ? (size_t)(end-line) : size-at;
        at+=len+1;
        if(len==0 || line[0]=='#') continue;
        const char *tab=memchr(line,'\t',len);
        if(tab==NULL) { printf("  a grammar vector with no tab\n"); bad++; continue; }
        size_t klen=(size_t)(tab-line);
        /* `escape:<hex>`: the field the line holds unescapes (rp_unescape_n,
         * one pass from the left) to exactly the bytes the hex spells. */
        if(klen>=7 && !memcmp(line,"escape:",7)) {
            const char *hex=line+7; size_t hlen=klen-7, got=0;
            char *want=malloc(hlen/2+1); bool good=hlen%2==0;
            for(size_t i=0;good && i<hlen;i+=2) {
                char pair[3]={hex[i],hex[i+1],0};
                good=isxdigit((unsigned char)pair[0]) && isxdigit((unsigned char)pair[1]);
                want[i/2]=(char)strtol(pair,NULL,16);
            }
            char *bytes=rp_unescape_n(tab+1,len-klen-1,true,&got);
            n++;
            if(!good || got!=hlen/2 || memcmp(bytes,want,got)) {
                printf("  grammar: `%.*s` does not unescape to %.*s\n",(int)(len-klen-1),tab+1,(int)hlen,hex);
                bad++;
            }
            free(bytes); free(want);
            continue;
        }
        /* `record:read` or `record:REFUSED <words>`: the field, its lines
         * joined by `¶`, is a record yonder wrote, which rp_record_read
         * reads whole, or refuses with a reason holding <words>. A
         * `replay:` vector is a replay's record, which only
         * tools/yonder_records.py reads. */
        if(klen>=7 && !memcmp(line,"replay:",7)) continue;
        if(klen>=7 && !memcmp(line,"record:",7)) {
            RpText whole={0};
            for(const char *p=tab+1, *e=line+len; p<e; ) {
                const char *mark=rp_find(p,(size_t)(e-p),"\xC2\xB6",2);
                const char *stop=mark!=NULL ? mark : e;
                rp_add(&whole,p,(size_t)(stop-p)); rp_add(&whole,"\n",1);
                p=mark!=NULL ? mark+2 : e;
            }
            RpRecord r;
            const char *why=rp_record_read(whole.p,whole.n,&r);
            bool want_read=klen==11 && !memcmp(line+7,"read",4);
            bool refused=klen>15 && !memcmp(line+7,"REFUSED ",8);
            n++;
            if(want_read ? why!=NULL : !refused || why==NULL || rp_find(why,strlen(why),line+15,klen-15)==NULL) {
                printf("  grammar: a record vector `%.*s` read as %s\n",(int)klen,line,why!=NULL ? why : "whole");
                bad++;
            }
            if(why==NULL) rp_record_free(&r);
            free(whole.p);
            continue;
        }
        bool index=klen>6 && !memcmp(line,"index:",6);
        RpLine l=rp_classify(tab+1,len-klen-1,index);
        n++;
        if(strlen(l.kind)!=klen || memcmp(l.kind,line,klen)) {
            char *shown=strndup(tab+1,len-klen-1);
            printf("  grammar: `%s` read as %s, not %.*s\n",rp_display(shown),l.kind,(int)klen,line);
            free(shown);
            bad++;
        }
    }
    free(text);
    if(n<100) { printf("  the grammar has only %u vectors\n",n); bad++; }
    return bad;
}

/* WHAT `--window` TAKES AND REFUSES: an agent holding commas, split only
 * where a known key follows; and, each refused, a key nobody states, one
 * stated twice, a view without its height, and a value no record says.
 * The problems found. */
static unsigned rp_window_cases(void) {
    unsigned bad=0;
    size_t agent=0, zoom=0, laid=0;
    for(size_t k=0;k<RP_STATABLE;k++) {
        if(kStatable[k].fact==F_AGENT) agent=k;
        if(kStatable[k].fact==F_ZOOM) zoom=k;
        if(kStatable[k].fact==F_LAID) laid=k;
    }
    if(rp_window_state("agent=Mozilla/5.0 (KHTML, like Gecko) X/1,zoom=1250,laid-out-at=10x20")!=NULL ||
       rp_stated[agent]==NULL || strcmp(rp_stated[agent],"Mozilla/5.0 (KHTML, like Gecko) X/1") ||
       rp_stated[zoom]==NULL || strcmp(rp_stated[zoom],"1250") ||
       rp_stated[laid]==NULL || strcmp(rp_stated[laid],"10x20 px")) {
        printf("  --window did not take an agent holding commas, a zoom and a view\n"); bad++;
    }
    rp_window_clear();
    static const char *const kRefused[]={"cookies=a=1","zoom=1000,zoom=1250","laid-out-at=2526","zoom=01000",
                                         "appearance=dim","zoom","tz="};
    for(size_t i=0;i<sizeof(kRefused)/sizeof(kRefused[0]);i++) {
        if(rp_window_state(kRefused[i])==NULL) { printf("  --window took `%s`\n",kRefused[i]); bad++; }
        rp_window_clear();
    }
    return bad;
}

/* THE CHILD'S WALL CLOCK (rp_wait reads its limit through rp_elapsed_ms):
 * a span with no borrow, two whose nanoseconds borrow across a second (a
 * page started late in one second and looked at early in the next; the
 * second, 49.5 ms, is one that a negative nanosecond quotient truncated
 * toward zero and wrapped back would read as 50), two equal readings, a
 * `to` before `from`, and a span past 2^32 ms. The problems found. */
static unsigned rp_elapsed_cases(void) {
    static const struct { struct timespec from, to; uint64_t ms; } kSpans[]={
        {{10,100000000},{12,600000000},2500},
        {{10,950000000},{11,50000000},100},
        {{10,950500000},{11,0},49},
        {{10,950000000},{10,950000000},0},
        {{11,50000000},{10,950000000},0},
        {{1,900000000},{4294969,196000000},UINT64_C(4294967296)},
    };
    unsigned bad=0;
    for(size_t i=0;i<sizeof(kSpans)/sizeof(kSpans[0]);i++) {
        uint64_t got=rp_elapsed_ms(&kSpans[i].from,&kSpans[i].to);
        if(got!=kSpans[i].ms) {
            printf("  elapsed: %lld.%09ld to %lld.%09ld read as %llu ms, not %llu\n",(long long)kSpans[i].from.tv_sec,
                   kSpans[i].from.tv_nsec,(long long)kSpans[i].to.tv_sec,kSpans[i].to.tv_nsec,
                   (unsigned long long)got,(unsigned long long)kSpans[i].ms);
            bad++;
        }
    }
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
        /* field-sizing is a property yonder does not know, so its MISSING
         * line is the proof this imported sheet was read. */
        {"https://cdn.fixture.test/i.css","p{margin:0;field-sizing:content}",NULL,""},
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
         "sheetUnread charsetLost","field-sizing m.js:",0,false,false,false},
        {"a text body","http://fixture.test/notes.txt","\xEF\xBB\xBFplain <b>text</b> caf\xC3\xA9\n",
         rp_head(200,"OK","text/plain","","http://fixture.test/notes.txt",false,WAY_BODY_TEXT),NULL,"body:",0,false,false,false},
        {"a file's page, its script, and a timer that fires after it is shown","file:///pages/f.html",
         "<!doctype html><p>a file</p><script src=f.js></script><script>fetch</script>",
         rp_head(0,"","","","",false,WAY_BODY_HTML),NULL,"f.js: #timer-1:",0,false,false,false},
        {"an error page","http://fixture.test/busy","<!doctype html><p>slow down</p>",
         rp_head(429,"Too Many Requests","text/html","","http://fixture.test/busy",false,WAY_BODY_HTML),
         NULL,"429",0,false,false,false},
        {"a script that did not come, a module, a global","http://fixture.test/gone",
         "<!doctype html><script src=gone.js></script><script type=module>1</script>"
         "<script>if(window.localStorage)x()</script><svg></svg>",
         rp_head(200,"OK","text/html","","http://fixture.test/gone",false,WAY_BODY_HTML),NULL,"gone.js",0,false,false,false},
        {"a page too deep to read to its end","http://fixture.test/deep",deep,
         rp_head(200,"OK","text/html","","http://fixture.test/deep",false,WAY_BODY_HTML),NULL,"to its end",0,false,false,false},
        {"a picture asked for by itself","http://fixture.test/shot.png","",
         rp_head(200,"OK","image/png","","http://fixture.test/shot.png",false,WAY_BODY_IMAGE),NULL,"still coming",0,false,false,false},
        {"a wide window, which a media query and a script both see","http://fixture.test/wide",
         "<!doctype html><style>@media (max-width:1000px){p{field-sizing:fixed}}</style><p id=p>wide</p>"
         "<script>if(document.getElementById('p').offsetWidth<1000)narrow()</script>",
         rp_head(200,"OK","text/html","","http://fixture.test/wide",false,WAY_BODY_HTML),
         "narrow field-sizing","1400 px",1400,false,false,false},
        {"a page read with scripts off","http://fixture.test/quiet",
         "<!doctype html><p>no script runs</p><script>nothere()</script>",
         rp_head(200,"OK","text/html","","http://fixture.test/quiet",false,WAY_BODY_HTML),
         "nothere","scripts:",0,true,false,false},
        /* Read with the modern switch off: the interfaces the arc adds are
         * not there, and a replay that read it with the switch on would
         * record another page. */
        {"a page read with modern off","http://fixture.test/plain-dom",
         "<!doctype html><p>no modern surface</p><script>Node</script>",
         rp_head(200,"OK","text/html","","http://fixture.test/plain-dom",false,WAY_BODY_HTML),
         NULL,"modern: off Node",0,false,true,false},
        /* An address holding `M%49SSING` of its own beside MISSING: the
         * record writes them apart, and the replay must ask for the script
         * by the same address the page did. */
        {"an address that spells a token both ways","http://fixture.test/M%49SSING/MISSING/p.html",
         "<!doctype html><p>percent</p><script src=s.js></script>",
         rp_head(200,"OK","text/html","","http://fixture.test/M%49SSING/MISSING/p.html",false,WAY_BODY_HTML),
         NULL,"M%2549SSING/M%49SSING/s.js: ran",0,false,false,false},
        /* Canvases, one a script makes, are one FAILED line, and a frame a
         * script inserts with its src property is no failure: a replay
         * writes the same lines. */
        {"canvases and an inserted frame","http://fixture.test/loud",
         "<!doctype html><canvas></canvas><canvas width=10 height=10></canvas><script>"
         "var f=document.createElement('iframe');f.src='/poll';document.body.appendChild(f);"
         "document.body.appendChild(document.createElement('canvas'))</script>",
         rp_head(200,"OK","text/html","","http://fixture.test/loud",false,WAY_BODY_HTML),
         "\nFAILED unsupported: iframe\nFAILED unsupported: frame",
         "\nFAILED unsupported: canvas (1)\nMISSING element canvas (3)",0,false,false,false},
        /* Read with every input unlike the harness's, its script writing
         * each into its record as the name of a global it looks for, so a
         * replay that takes the harness's value for any of them writes
         * another record. */
        {"inputs unlike the harness's","http://fixture.test/other",
         "<!doctype html><style>#dk,#mo{width:10px}"
         "@media (prefers-color-scheme: dark){#dk{width:111px}}"
         "@media (prefers-reduced-motion: reduce){#mo{width:122px}}"
         "#vw{width:100vw;height:1px}#vh{height:50vh}"
         "@keyframes grow{from{width:10px}to{width:200px}}#an{animation:grow 1s forwards;height:1px}</style>"
         "<div id=dk></div><div id=mo></div><div id=vw></div><div id=vh></div><div id=an></div><script>"
         "function say(k,v){try{eval(k+'_'+String(v).replace(/\\W/g,'_'))}catch(e){}}"
         "function w(id){return document.getElementById(id).offsetWidth}"
         "say('agent',navigator.userAgent);say('cookie',document.cookie);say('clock',Date.now());"
         "say('zone',new Date(0).getTimezoneOffset());say('dark',w('dk'));say('motion',w('mo'));"
         "say('wide',w('vw'));say('high',document.getElementById('vh').offsetHeight);"
         "setTimeout(function(){say('animate',w('an'))},2000)</script>",
         rp_head(200,"OK","text/html","","http://fixture.test/other",false,WAY_BODY_HTML),
         NULL,"agent_other_agent_2 cookie_kept_yes clock_1600000000123 zone_120 dark_111 motion_122 wide_720 high_280 "
         "animate_704",0,false,false,true},
    };
    const char *dirs[]={"tools/yonder_loop_fixture","tools/yonder_script_fixture","tools/pages"};
    unsigned pages=0, bad=0;
    uint32_t seq=0;
    char name[256], first_hand[256]="", other_hand[256]="";
    char *said=rp_cat(work,"/fixtures","");
    /* Each page loaded as a person browsing with keep_pages on would load
     * it, and what yonder kept written out, IN A CHILD OF ITS OWN (REPLAY.md
     * § 6): the window and its fakes run there, so every process that
     * replays a page starts from a parent none of that has touched. The
     * child says how many pages it kept, the first hand-made page's name
     * and that of the page read with inputs unlike the harness's in
     * WORK-DIR/fixtures, and exits with its problems. */
    fflush(stdout); fflush(stderr);
    pid_t keeper=fork();
    if(keeper<0) { printf("  no process to keep the fixtures in\n"); free(said); return 2; }
    if(keeper==0) {
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
                }
                load.address=address; load.seq=++seq; load.local=!strncmp(address,"file://",7);
                load.scripts=set<3 || !hand[i].scripts_off;
                /* The window a person browsing has: the harness's view, unless
                 * the page asks for a width of its own, and every setting at
                 * its default. Its record says each, and the replay reads the
                 * page in the window it says. */
                load.width=set<3 || hand[i].width==0 ? 800 : hand[i].width; load.height=600;
                load.zoom=1000; load.modern=set<3 || !hand[i].modern_off; load.script_seconds=YONDER_SCRIPT_SECONDS_DEFAULT;
                load.dir="/tmp/kept"; load.keep=true; load.budget_ms=60000;
                rp_harness_inputs(&load);
                if(set==3 && hand[i].other) rp_other_inputs(&load);
                fake_reset(); snprintf(fake_dirs[nfake_dirs++],256,"/tmp/kept");
                size_t live_before=live;
                RpRan ran=rp_run(&load,rp_site_lookup,&site,NULL);
                pages++;
                size_t wlen;
                const char *record=rp_written("/tmp/kept",address,load.seq,name,sizeof(name),&wlen);
                if(set==3 && i==0 && record!=NULL) snprintf(first_hand,sizeof(first_hand),"%s",name);
                if(set==3 && hand[i].other && record!=NULL) snprintf(other_hand,sizeof(other_hand),"%s",name);
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
                    bool lines=list!=NULL && list[0]=='\n';
                    char *words=list!=NULL ? strdup(list) : NULL;
                    for(char *w=words!=NULL ? strtok(words,lines ? "\n" : " ") : NULL; w!=NULL;
                        w=strtok(NULL,lines ? "\n" : " "))
                        if(record==NULL || (lines ? rp_has_line_start(record,w) : strstr(record,w)!=NULL)!=(must==1)) {
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
        char kept_said[600];
        snprintf(kept_said,sizeof(kept_said),"%u\n%s\n%s\n",pages,first_hand,other_hand);
        if(!rp_put(said,kept_said,strlen(kept_said))) bad++;
        bad+=rp_faults;
        fflush(stdout); fflush(stderr);
        _exit(bad>63 ? 63 : (int)bad);
    }
    int status=0; pid_t got=-1;
    while((got=waitpid(keeper,&status,0))<0 && errno==EINTR) {}
    if(got!=keeper) { printf("  the fixtures' child could not be waited for\n"); bad++; }
    else if(!WIFEXITED(status) || WEXITSTATUS(status)!=0) {
        printf("  keeping the fixtures came to %s %d\n",WIFEXITED(status) ? "problems:" : "a death: signal",
               WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
        bad++;
    }
    {
        char *text=rp_file(said,NULL), *nl=text!=NULL ? strchr(text,'\n') : NULL;
        char *second=nl!=NULL ? strchr(nl+1,'\n') : NULL;
        if(second==NULL || !rp_decimal(text,(size_t)(nl-text),&pages) || second-nl<3 || strlen(second+1)<2) {
            printf("  the fixtures' child did not say what it kept\n"); bad++;
        } else {
            snprintf(first_hand,sizeof(first_hand),"%.*s",(int)(second-nl-1),nl+1);
            snprintf(other_hand,sizeof(other_hand),"%.*s",(int)strcspn(second+1,"\n"),second+1);
        }
        free(text);
    }
    free(said);
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
    bad+=rp_corpus_cases(work,kept,first_hand,other_hand);
    bad+=rp_grammar_vectors();
    bad+=rp_window_cases();
    bad+=rp_elapsed_cases();
    bad+=rp_faults;
    printf("replay self-test: %u pages kept and replayed, %u problems\n",pages,bad);
    return bad!=0;
}

int main(int argc, char **argv) {
    int rc;
    /* A SIGCHLD the runner was started ignoring would make the kernel reap
     * each page's child itself, and the status that says what the page
     * came to would be lost (REPLAY.md § 6). */
    signal(SIGCHLD,SIG_DFL);
    if(argc==3 && !strcmp(argv[1],"--self-test")) {
        rc=rp_self_test(argv[2]);
    } else if(argc>=3) {
        uint32_t seconds=60; const char *only=NULL;
        for(int i=3;i<argc;i++) {
            if(!strcmp(argv[i],"--window") && i+1<argc) {
                const char *bad=rp_window_state(argv[++i]);
                if(bad!=NULL) { fprintf(stderr,"replay: %s\n",rp_display(bad)); return 2; }
                continue;
            }
            if(!strcmp(argv[i],"--seconds") && i+1<argc) {
                ++i;
                if(!rp_decimal(argv[i],strlen(argv[i]),&seconds)) {
                    fprintf(stderr,"replay: --seconds takes a whole number of seconds, not %s\n",rp_display(argv[i]));
                    return 2;
                }
            }
            else if(!strcmp(argv[i],"--only") && i+1<argc) only=argv[++i];
            else if(!strcmp(argv[i],"--crash-page") && i+1<argc) rp_crash_page=argv[++i];
            else { fprintf(stderr,"replay: what is %s?\n",argv[i]); return 2; }
        }
        RpTally t={0};
        rc=rp_replay_dir(argv[1],argv[2],(uint64_t)seconds*1000,only,&t);
    } else {
        fprintf(stderr,"usage: %s KEPT-DIR OUT-DIR [--seconds N] [--only TEXT] [--window KEY=VALUE,...] [--crash-page NAME]\n"
                       "       %s --self-test WORK-DIR\n",argv[0],argv[0]);
        return 2;
    }
    if(live!=0) fprintf(stderr,"replay: %zu blocks still held at the end\n",live);
    return rc!=0 ? rc : failures!=0 || live!=0;
}
