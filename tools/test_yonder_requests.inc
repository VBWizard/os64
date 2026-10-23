/* A SCRIPT'S REQUESTS, END TO END (MODERN_WEB.md § Slice 2): a page's
 * fetch() and XMLHttpRequest through yonder's window, the real fetch job
 * (userland/apps/yonder/fetch_job.c) run against the fake libfetch
 * (test_fetch_fake.inc) when a case lands it, its answer the page's task.
 * What reaches the wire (method, Origin, cookies, the preflight), what the
 * page sees, what the page's record says, and what a departure cancels. */

/* The worker's part for the request whose address ends with `tail`: the
 * fetch job run for real, then reaped as the pool would. False when no such
 * request is out. */
static bool request_land(const char *tail) {
    for(int i=0;i<nrequest_jobs;i++) {
        const char *url=request_jobs[i].job->url;
        size_t n=strlen(url), t=strlen(tail);
        if(n<t || strcmp(url+n-t,tail)!=0) continue;
        os64_work_id_t id=request_jobs[i].id; yonder_fetch_job_t *job=request_jobs[i].job;
        request_jobs[i]=request_jobs[--nrequest_jobs];
        void *got=NULL;
        (void)yonder_fetch_run(job,NULL,NULL,&got);
        reaped(id,job,got);
        return true;
    }
    return false;
}

/* The page's record, rendered, into a static buffer. */
static const char *request_record_text(void) {
    static char text[16384];
    text[0]='\0';
    if(g.page.diag!=NULL) yonder_diag_render(g.page.diag,text,sizeof(text));
    return text;
}

static void request_drop(void) {
    while(nrequest_jobs>0) { nrequest_jobs--; yonder_fetch_release(request_jobs[nrequest_jobs].job,NULL); }
    loop_drop();
    fake_fetch_reset();
}

/* Same origin: a JSON answer reaches the page as a task, and the record
 * has its line. */
static void request_same_origin_case(void) {
    fake_fetch_reset();
    fake_answer("GET","http://fixture.test/data.json",200,"Content-Type: application/json\r\n","{\"v\":\"got\"}");
    loop_page("<p id=out>x</p><script>fetch('/data.json').then(r=>r.json()).then(j=>{"
        "document.getElementById('out').textContent=j.v});</script>");
    check(loop_settle() && g.page.tree!=NULL && nrequest_jobs==1,"requests: a page's fetch is a job on the pool");
    check(request_land("/data.json") && loop_settle(),"requests: the job runs and is reaped");
    check(script_rebuild() && probe_text_is("out","got"),"requests: the answer reaches the page as its task");
    char origin[64];
    check(nfake_requests==1 && !strcmp(fake_requests[0].method,"GET") &&
          fake_field(0,"Origin",origin,sizeof(origin))[0]=='\0',
          "requests: a same-origin GET carries no Origin");
    check(strstr(request_record_text(),"request GET http://fixture.test/data.json: 200 basic, 0 hops")!=NULL,
          "requests: the page's record has the request's line, with its hops");
    request_drop();
}

/* Cross origin: a CORS answer the page may read, one it may not, and a
 * preflight before a PUT. */
static void request_cors_case(void) {
    fake_fetch_reset();
    fake_answer("GET","http://other.test/open",200,"Access-Control-Allow-Origin: *\r\nX-Secret: s\r\n","open");
    fake_answer("GET","http://other.test/shut",200,"","shut");
    fake_answer("OPTIONS","http://other.test/put",204,
                "Access-Control-Allow-Origin: http://fixture.test\r\nAccess-Control-Allow-Methods: PUT\r\n",NULL);
    fake_answer("PUT","http://other.test/put",200,"Access-Control-Allow-Origin: http://fixture.test\r\n","put");
    loop_page("<p id=a>x</p><p id=b>x</p><p id=c>x</p><script>"
        "function say(id,t){document.getElementById(id).textContent=t}"
        "fetch('http://other.test/open').then(r=>r.text().then(t=>say('a',r.type+' '+t+' '+r.headers.get('x-secret'))));"
        "fetch('http://other.test/shut').then(()=>say('b','read'),e=>say('b',e.name));"
        "fetch('http://other.test/put',{method:'PUT',body:'p'}).then(r=>r.text()).then(t=>say('c',t));</script>");
    check(loop_settle() && nrequest_jobs==3,"requests: three cross-origin fetches out");
    check(request_land("/open") && request_land("/shut") && request_land("/put") && loop_settle() && script_rebuild(),
          "requests: each answered");
    check(probe_text_is("a","cors open null"),"requests: a CORS answer is read, its unexposed header hidden");
    check(probe_text_is("b","TypeError"),"requests: an answer that does not allow the origin is a TypeError");
    check(probe_text_is("c","put"),"requests: a PUT across origins is preflighted and sent");
    char origin[64];
    bool preflighted=false;
    for(int i=0;i+1<nfake_requests;i++)
        if(!strcmp(fake_requests[i].method,"OPTIONS") && !strcmp(fake_requests[i+1].method,"PUT") &&
           !strcmp(fake_field(i+1,"Origin",origin,sizeof(origin)),"http://fixture.test"))
            preflighted=true;
    check(preflighted,"requests: the preflight goes first, and the PUT names the page's origin");
    const char *record=request_record_text();
    check(strstr(record,"FAILED request: GET http://other.test/shut: ")!=NULL,
          "requests: a CORS failure is a FAILED line in the page's record");
    request_drop();
}

/* XMLHttpRequest, asynchronous and synchronous, through the same job. */
static void request_xhr_case(void) {
    fake_fetch_reset();
    fake_answer("GET","http://fixture.test/a.txt",200,"Content-Type: text/plain\r\n","async");
    fake_answer("GET","http://fixture.test/s.txt",200,"Content-Type: text/plain\r\n","sync");
    loop_page("<p id=a>x</p><p id=s>x</p><script>"
        "var x=new XMLHttpRequest();x.open('GET','/a.txt');var seen=[];"
        "x.onreadystatechange=()=>seen.push(x.readyState);"
        "x.onload=()=>{document.getElementById('a').textContent=seen.join('')+' '+x.responseText};x.send();"
        "var y=new XMLHttpRequest();y.open('GET','/s.txt',false);y.send();"
        "document.getElementById('s').textContent=y.status+' '+y.responseText;</script>");
    check(loop_settle() && script_rebuild() && probe_text_is("s","200 sync"),
          "requests: a synchronous XMLHttpRequest runs the job on the window thread");
    check(request_land("/a.txt") && loop_settle() && script_rebuild() && probe_text_is("a","234 async"),
          "requests: an asynchronous one fires its events as its answer's task");
    const char *record=request_record_text();
    check(strstr(record,"request GET http://fixture.test/s.txt: 200 basic, 0 hops, synchronous")!=NULL,
          "requests: the record says a synchronous request stopped the page");
    request_drop();
}

/* Aborting cancels the job; the page departing cancels what is out. */
static void request_cancel_case(void) {
    fake_fetch_reset();
    unsigned cancels=request_cancels;
    loop_page("<p id=out>x</p><script>var c=new AbortController();"
        "fetch('/slow',{signal:c.signal}).catch(e=>{document.getElementById('out').textContent=e.name});"
        "fetch('/left');setTimeout(()=>c.abort(),10);</script>");
    check(loop_settle() && nrequest_jobs==2,"requests: two out");
    now_ms+=20;
    check(script_turn() && script_rebuild() && probe_text_is("out","AbortError") && request_cancels==cancels+1,
          "requests: an abort rejects the fetch and cancels its job");
    page_departs();
    check(request_cancels==cancels+2 && nrequest_jobs==0,"requests: the page departing cancels what is out");
    request_drop();
}

static void request_cases(void) {
    request_same_origin_case();
    request_cors_case();
    request_xhr_case();
    request_cancel_case();
}
