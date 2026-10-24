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

/* The fake's request for the address ending with `tail`, -1 for none. */
static int request_sent(const char *tail) {
    for(int i=0;i<nfake_requests;i++) {
        size_t n=strlen(fake_requests[i].url), t=strlen(tail);
        if(n>=t && !strcmp(fake_requests[i].url+n-t,tail)) return i;
    }
    return -1;
}

/* A REQUEST'S ORIGIN IS ITS OWN PAGE'S, at every moment of the page's
 * life, and never the page on screen's (here, none). A page asked for as
 * http:// that a redirect sent to https:// asks for its own files by
 * relative address: at parse, at DOMContentLoaded (while stream_finish
 * holds the page, as g.finishing, between the stream and the screen), at
 * load, and from a timer. Each is same-origin, so basic, with no Origin
 * and the page's own address as its Referer; a <script src> its
 * DOMContentLoaded adds names the same page, and so does a synchronous
 * XMLHttpRequest. */
static void request_redirected_page_case(void) {
    fake_fetch_reset();
    static const char *const kMoments[]={"parse","ready","load","timer","sync"};
    for(size_t i=0;i<sizeof(kMoments)/sizeof(kMoments[0]);i++) {
        static char urls[5][64];
        snprintf(urls[i],sizeof(urls[i]),"https://fixture.test/%s.json",kMoments[i]);
        fake_answer("GET",urls[i],200,"Content-Type: application/json\r\n","{}");
    }
    loop_asked="http://fixture.test"; loop_final="https://fixture.test/"; loop_encrypted=true;
    loop_page("<p id=out>x</p><script>var seen=[];"
        "function say(){seen.sort();document.getElementById('out').textContent=seen.join(' ')}"
        "function get(n){fetch('/'+n+'.json',{cache:'no-store'}).then(r=>{seen.push(n+'='+r.type);say()},"
        "e=>{seen.push(n+'='+e.name);say()})}"
        "get('parse');"
        "document.addEventListener('DOMContentLoaded',()=>{get('ready');"
        "var s=document.createElement('script');s.src='/late.js';document.head.appendChild(s);"
        "var x=new XMLHttpRequest();x.open('GET','/sync.json',false);"
        "try{x.send();seen.push('sync='+x.status)}catch(e){seen.push('sync='+e.name)}});"
        "window.addEventListener('load',()=>get('load'));"
        "setTimeout(()=>get('timer'),10);</script>");
    loop_asked=loop_final=NULL; loop_encrypted=false;
    check(loop_settle() && g.page.tree!=NULL && os64_streq(g.page.way.url,"https://fixture.test/") &&
          nrequest_jobs==3,"requests: a redirected page's fetches at parse, DOMContentLoaded and load are out");
    bool late=false;
    for(int i=0;i<nscript_jobs;i++) {
        const yonder_script_job_t *job=script_jobs[i].job;
        if(os64_streq(job->url,"https://fixture.test/late.js"))
            late=os64_streq(job->hooks.referrer,"https://fixture.test/");
    }
    check(late,"requests: a script its DOMContentLoaded adds is fetched with the page's own address as Referer");
    now_ms+=20;
    check(script_turn() && nrequest_jobs==4,"requests: and its timer's");
    for(int i=0;i<nrequest_jobs;i++) {
        char origin[WAY_ORIGIN_TEXT_MAX];
        way_origin_serialize(&request_jobs[i].job->request.origin,origin,sizeof(origin));
        char what[160];
        snprintf(what,sizeof(what),"requests: %s's origin is the page's own, after its redirect",
                 request_jobs[i].job->url);
        check(os64_streq(origin,"https://fixture.test"),what);
    }
    check(request_land("/parse.json") && request_land("/ready.json") && request_land("/load.json") &&
          request_land("/timer.json") && loop_settle() && script_rebuild(),"requests: each is answered");
    check(probe_text_is("out","load=basic parse=basic ready=basic sync=200 timer=basic"),
          "requests: every one is same-origin, read as basic");
    for(size_t m=0;m<sizeof(kMoments)/sizeof(kMoments[0]);m++) {
        char tail[32], origin[64], referer[96], what[160];
        snprintf(tail,sizeof(tail),"/%s.json",kMoments[m]);
        int i=request_sent(tail);
        fake_field(i,"Origin",origin,sizeof(origin));
        fake_field(i,"Referer",referer,sizeof(referer));
        snprintf(what,sizeof(what),"requests: the %s fetch carries no Origin and the page's address as Referer",
                 kMoments[m]);
        check(i>=0 && origin[0]=='\0' && os64_streq(referer,"https://fixture.test/"),what);
    }
    const char *record=request_record_text();
    check(strstr(record,"request GET https://fixture.test/ready.json: 200 basic, 0 hops")!=NULL &&
          strstr(record,"request GET https://fixture.test/sync.json: 200 basic, 0 hops, synchronous")!=NULL &&
          strstr(record,"FAILED request")==NULL,
          "requests: the page's record has them as basic, the synchronous one included");
    /* A serial no page has is a page gone: what it asks for is refused,
     * never sent as the page on screen's. */
    uint64_t gone=g.page.serial+1000;
    os64_dom_request_t q={.url="https://fixture.test/gone.json",.method="GET",.headers=""};
    os64_dom_response_t answer;
    int jobs=nrequest_jobs, scripts=nscript_jobs, sent=nfake_requests;
    check(script_request(NULL,gone,1,&q)==0 && !script_run(NULL,gone,&q,NULL,NULL,&answer) &&
          script_fetch(NULL,gone,1,"https://fixture.test/gone.js","")==0 &&
          nrequest_jobs==jobs && nscript_jobs==scripts && nfake_requests==sent,
          "requests: a page that is gone has its requests refused");
    request_drop();
}

/* A PAGE'S NAVIGATION IS ITS OWN too: the page that asks is named in its
 * Referer, judged for a downgrade, and given the line a refusal writes,
 * wherever that page is in its arrival — never the page on screen, which
 * here is an http:// page A while the asking page B is https://. */

/* Page A on screen, then B arriving with `rest`, its body whole when
 * `done`. */
static void nav_from_b(const char *rest, bool done) {
    loop_final="http://fixture.test/a";
    loop_page("<p>A</p>");
    loop_final=NULL;
    check(loop_settle() && view_turns() && os64_streq(g.page.way.url,"http://fixture.test/a"),"navigations: A on screen");
    start_trip("https://fixture.test/b",NULL,NAV_GO,NULL);
    char *html=pad_page(rest);
    way_head_t h=stream_head("text/html","",false);
    os64_strcopy(h.url,sizeof(h.url),"https://fixture.test/b");
    h.encrypted=true;
    yonder_mail_post_head(g.stream.mail,&h);
    stream_post_body((const uint8_t *)html,strlen(html),YONDER_STREAM_CHUNK);
    if(done) stream_post_verdict(true,OS64_FETCH_OK,"");
    free(html);
}

/* The navigation the window sent last: its address and its Referer. */
static bool nav_sent(const char *url, const char *referrer) {
    const yonder_trip_t *trip=pool_work.job;
    return trip!=NULL && trip->kind==YONDER_JOB_TRIP && os64_streq(trip->url,url) &&
           os64_streq(trip->referrer,referrer);
}

static void nav_referrer_case(const char *when, const char *rest, bool waits) {
    char what[160];
    nav_from_b(rest,true);
    loop_settle();
    if(waits) {
        check(g.coming.active,"navigations: B waits for its sheet");
        now_ms+=50;
        loop_settle();
    }
    snprintf(what,sizeof(what),"navigations: location.href %s names B as its Referer, not the page on screen",when);
    check(nav_sent("https://fixture.test/next","https://fixture.test/b"),what);
    loop_drop();
}

static void nav_refused_case(const char *when, const char *rest) {
    char what[160];
    nav_from_b(rest,true);
    snprintf(what,sizeof(what),"navigations: a refused location.href %s is a line in B's record",when);
    check(loop_settle() && view_turns() && os64_streq(g.page.way.url,"https://fixture.test/b") &&
          strstr(request_record_text(),"FAILED link refused: ")!=NULL,what);
    loop_drop();
}

static void navigation_cases(void) {
    nav_referrer_case("from the parse","<script>location.href='/next'</script><p>tail</p>",false);
    nav_referrer_case("at DOMContentLoaded",
        "<script>document.addEventListener('DOMContentLoaded',()=>{location.href='/next'})</script>",false);
    nav_referrer_case("while B waits for its sheets",
        "<script>setTimeout(()=>{location.href='/next'},10)</script><link rel=stylesheet href=wait.css>",true);
    nav_refused_case("from the parse","<script>location.href='ftp://fixture.test/f'</script><p>tail</p>");
    nav_refused_case("at DOMContentLoaded",
        "<script>document.addEventListener('DOMContentLoaded',()=>{location.href='ftp://fixture.test/f'})</script>");
    /* B, encrypted, sends the person to an address in the clear while it
     * is still arriving: that is a downgrade from B, whatever A is, so it
     * is asked first, and a yes sends it with B's Referer. */
    nav_from_b("<script>location.href='http://fixture.test/plain'</script><p>tail</p>",false);
    check(loop_settle() && g.stream.active && g.asker==ASK_REQUEST && g.pending.downgrade,
          "navigations: B's location.href to http:// is a downgrade from B, and asked about");
    bar_answered(true);
    check(nav_sent("http://fixture.test/plain","https://fixture.test/b"),
          "navigations: the yes sends it with B's Referer");
    loop_drop();
    /* A person's click on B once it is shown names B, as it always has. */
    nav_from_b("<a id=l href=/next>go</a>",true);
    check(loop_settle() && view_turns() && os64_streq(g.page.way.url,"https://fixture.test/b"),"navigations: B shown");
    input_click_at("l");
    check(nav_sent("https://fixture.test/next","https://fixture.test/b"),"navigations: a click on B names B");
    loop_drop();
}

static void request_cases(void) {
    navigation_cases();
    request_redirected_page_case();
    request_same_origin_case();
    request_cors_case();
    request_xhr_case();
    request_cancel_case();
}
