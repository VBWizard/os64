/* test_fetch_fake.inc — libfetch's contract (fetch/fetch.h), faked, for the
 * hosts that run the fetch job (yonder/fetch_job.c): its own suite
 * (tools/test_yonder_fetch_job_host.c), the scripted-page host and the WPT
 * runner. libfetch's own behaviour is proved against a scripted peer by
 * tools/test_fetch_host.c; this fake keeps the parts of the contract the job
 * relies on, and RECORDS every request, so a case can say which requests a
 * page made: the method, the fields, the Origin and the cookies.
 *
 *   - headers_for is asked before the "dial", with the hop's address and
 *     whether its transport is encrypted (an https address is, here);
 *   - every final-head field reaches on_header in order, an omitted line by
 *     its name with a NULL value, and each Set-Cookie reaches on_set_cookie,
 *     all inside open;
 *   - a request with a method_name follows no redirect: a 301, 302, 303,
 *     307 or 308 ends as REDIRECT_STOPPED with its head readable;
 *   - a HEAD's answer and a 204, 205 or 304 have no body;
 *   - max_body is refused on the read that would pass it, as LIMIT.
 *
 * An answer comes from fake_answers (the first whose method, when it names
 * one, and address match), else from fake_resolve when a host set one; an
 * address with neither is a DIAL_FAILED fetch. */

#include "fetch/fetch.h"

typedef struct {
    const char *method;         /* NULL: any */
    const char *url;            /* the whole address, no fragment */
    int32_t status;
    const char *reason;
    const char *headers;        /* "Name: value\r\n" lines, wire order */
    const char *omitted;        /* "name\r\n" lines read by name alone */
    const char *body;
    size_t body_len;            /* 0 with a body: its strlen */
} fake_answer_t;

#define FAKE_ANSWERS 64
static fake_answer_t fake_answers[FAKE_ANSWERS];
static int nfake_answers;
/* A host's own answers (the WPT runner's checkout). `scratch` holds
 * whatever the answer points into, the fake's to free after the fetch. */
static bool (*fake_resolve)(const char *method, const char *url, fake_answer_t *out, char **scratch);

typedef struct {
    char method[64];
    char url[512];
    char fields[1100];          /* headers_for's lines: Cookie, Referer */
    char extra[9000];           /* extra_headers */
    char body_headers[1024];
    char accept[512];
    bool identity;
    char body[256];
    size_t body_len;
} fake_request_t;

#define FAKE_REQUESTS 64
static fake_request_t fake_requests[FAKE_REQUESTS];
static int nfake_requests;
static int fake_reads, fake_closes, fake_opens;

static void fake_fetch_reset(void) {
    memset(fake_answers,0,sizeof(fake_answers)); nfake_answers=0;
    memset(fake_requests,0,sizeof(fake_requests)); nfake_requests=0;
    fake_reads=fake_closes=fake_opens=0;
    fake_resolve=NULL;
}

static fake_answer_t *fake_answer(const char *method, const char *url, int32_t status, const char *headers,
                                  const char *body) {
    fake_answer_t *a=&fake_answers[nfake_answers++];
    memset(a,0,sizeof(*a));
    a->method=method; a->url=url; a->status=status; a->reason=status==200?"OK":"Fine";
    a->headers=headers?headers:""; a->body=body;
    return a;
}

/* The fields of request `i` that a case asks about, "" when it has none. */
static const char *fake_field(int i, const char *name, char *out, size_t cap) {
    out[0]='\0';
    if(i<0 || i>=nfake_requests) return out;
    const char *blocks[3]={fake_requests[i].fields,fake_requests[i].extra,fake_requests[i].body_headers};
    size_t n=strlen(name);
    for(int b=0;b<3;b++) {
        for(const char *p=blocks[b];*p;) {
            const char *colon=strchr(p,':'), *end=strstr(p,"\r\n");
            if(colon==NULL || end==NULL) break;
            if((size_t)(colon-p)==n && strncasecmp(p,name,n)==0) {
                snprintf(out,cap,"%.*s",(int)(end-colon-2),colon+2);
                return out;
            }
            p=end+2;
        }
    }
    return out;
}

struct os64_fetch {
    fake_answer_t a;
    char *scratch;
    size_t at, body_len;
    bool bodiless, over;
    uint64_t max_body;
    os64_fetch_head_t head;
    os64_fetch_status_t status;
    os64_fetch_detail_t detail;
    char location[2048];
    char reason[256];
};

static bool fake_redirect(int32_t s) { return s==301||s==302||s==303||s==307||s==308; }

os64_fetch_t *os64_fetch_open(const char *url, const os64_fetch_options_t *opt) {
    os64_fetch_t *f=os64_calloc(1,sizeof(*f));
    if(f==NULL) return NULL;
    fake_opens++;
    const char *method=opt->method_name?opt->method_name:opt->method==OS64_FETCH_METHOD_POST?"POST":"GET";
    if(nfake_requests<FAKE_REQUESTS) {
        fake_request_t *q=&fake_requests[nfake_requests++];
        snprintf(q->method,sizeof(q->method),"%s",method);
        snprintf(q->url,sizeof(q->url),"%s",url);
        snprintf(q->extra,sizeof(q->extra),"%s",opt->extra_headers?opt->extra_headers:"");
        snprintf(q->body_headers,sizeof(q->body_headers),"%s",opt->body_headers?opt->body_headers:"");
        snprintf(q->accept,sizeof(q->accept),"%s",opt->accept?opt->accept:"");
        q->identity=opt->identity;
        q->body_len=opt->body_len;
        if(opt->body) memcpy(q->body,opt->body,opt->body_len<sizeof(q->body)?opt->body_len:sizeof(q->body)-1);
    }
    os64_url_t u;
    bool parsed=os64_url_parse(url,&u)==OS64_URL_OK;
    bool encrypted=parsed && !strcmp(u.scheme,"https");
    if(parsed && u.port==0) u.port=encrypted?443:80;
    if(opt->cancelled && opt->cancelled(opt->ctx)) { f->status=OS64_FETCH_INTERRUPTED; return f; }
    if(opt->headers_for && parsed) {
        char fields[OS64_FETCH_HOP_FIELDS_MAX];
        fields[0]='\0';
        if(!opt->headers_for(opt->ctx,&u,encrypted,fields,sizeof(fields))) {
            f->status=OS64_FETCH_REQUEST_FAILED; return f;
        }
        if(nfake_requests<=FAKE_REQUESTS) snprintf(fake_requests[nfake_requests-1].fields,sizeof(fake_requests[0].fields),"%s",fields);
    }
    bool found=false;
    for(int i=0;i<nfake_answers && !found;i++)
        if((fake_answers[i].method==NULL || !strcmp(fake_answers[i].method,method)) && !strcmp(fake_answers[i].url,url)) {
            f->a=fake_answers[i]; found=true;
        }
    if(!found && fake_resolve!=NULL) found=fake_resolve(method,url,&f->a,&f->scratch);
    if(!found) { f->status=OS64_FETCH_DIAL_FAILED; snprintf(f->reason,sizeof(f->reason),"fake: no answer for %s %s",method,url); return f; }
    if(f->a.body && f->a.body_len==0) f->a.body_len=strlen(f->a.body);
    /* The head, line by line, as libfetch hands it over. */
    for(const char *p=f->a.headers?f->a.headers:"";*p;) {
        const char *colon=strchr(p,':'), *end=strstr(p,"\r\n");
        if(colon==NULL || end==NULL) break;
        const char *v=colon+1; while(*v==' ') v++;
        size_t nlen=(size_t)(colon-p), vlen=(size_t)(end-v);
        if(opt->on_header) opt->on_header(opt->ctx,p,nlen,v,vlen);
        if(nlen==10 && !strncasecmp(p,"set-cookie",10) && opt->on_set_cookie && parsed)
            opt->on_set_cookie(opt->ctx,&u,encrypted,v,vlen);
        if(nlen==8 && !strncasecmp(p,"location",8)) {
            snprintf(f->location,sizeof(f->location),"%.*s",(int)vlen,v);
            f->head.has_location=true;
        }
        p=end+2;
    }
    for(const char *p=f->a.omitted?f->a.omitted:"";*p;) {
        const char *end=strstr(p,"\r\n");
        if(end==NULL) break;
        if(opt->on_header) opt->on_header(opt->ctx,p,(size_t)(end-p),NULL,0);
        p=end+2;
    }
    f->head.status=f->a.status;
    snprintf(f->head.reason,sizeof(f->head.reason),"%s",f->a.reason?f->a.reason:"");
    f->head.location=f->location;
    f->head.encrypted=encrypted;
    snprintf(f->head.url_text,sizeof(f->head.url_text),"%s",url);
    f->bodiless=!strcmp(method,"HEAD") || f->a.status==204 || f->a.status==205 || f->a.status==304;
    f->max_body=opt->max_body;
    f->body_len=f->bodiless?0:f->a.body_len;
    f->status=opt->method_name && fake_redirect(f->a.status) ? OS64_FETCH_REDIRECT_STOPPED : OS64_FETCH_OK;
    return f;
}
os64_fetch_status_t os64_fetch_status(const os64_fetch_t *f) { return f->status; }
const os64_fetch_detail_t *os64_fetch_detail(const os64_fetch_t *f) { return &f->detail; }
const os64_fetch_head_t *os64_fetch_head(const os64_fetch_t *f) {
    return f->status==OS64_FETCH_OK || f->status==OS64_FETCH_REDIRECT_STOPPED ? &f->head : NULL;
}
int64_t os64_fetch_read(os64_fetch_t *f, void *buf, size_t cap) {
    fake_reads++;
    if(f->over) return f->status==OS64_FETCH_OK||f->status==OS64_FETCH_REDIRECT_STOPPED ? 0 : -1;
    if(cap==0) return -1;
    /* The status is the last outcome's: a read's, once one is made. */
    size_t left=f->body_len-f->at;
    if(left==0) { f->over=true; f->status=OS64_FETCH_OK; return 0; }
    size_t n=cap<left?cap:left;
    if(f->max_body && f->at+n>f->max_body) {
        if(f->at>=f->max_body) { f->over=true; f->status=OS64_FETCH_LIMIT; return -1; }
        n=(size_t)(f->max_body-f->at);
    }
    memcpy(buf,f->a.body+f->at,n); f->at+=n;
    return (int64_t)n;
}
const char *os64_fetch_reason(os64_fetch_t *f) { return f->reason[0]?f->reason:"fake: the fetch failed"; }
void os64_fetch_close(os64_fetch_t *f) {
    if(f==NULL) return;
    fake_closes++;
    free(f->scratch);
    os64_free(f);
}
