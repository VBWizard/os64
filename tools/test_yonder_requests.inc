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
    check(strstr(request_record_text(),"request GET http://fixture.test/data.json: 200 basic, 0 hops, 11 bytes in 0 ms\n")!=NULL,
          "requests: the page's record has the request's line, with its hops, its bytes and its time");
    request_drop();
}

/* With the clock moving, a request's line says how long it waited for a
 * worker and how long its answer waited for the window. */
static void request_waits_case(void) {
    fake_fetch_reset();
    fake_answer("GET","http://fixture.test/w.json",200,"Content-Type: application/json\r\n","{}");
    loop_page("<script>fetch('/w.json')</script>");
    check(loop_settle() && nrequest_jobs==1,"requests: the fetch is out");
    clock_step=1000;
    bool landed=request_land("/w.json");
    clock_step=0;
    const char *text=request_record_text();
    const char *line=strstr(text,"request GET http://fixture.test/w.json: ");
    check(landed && line!=NULL && strstr(line,", queued ")!=NULL && strstr(line," ms later")!=NULL &&
          strstr(line,", queued ")<strchr(line,'\n') && strstr(line," ms later")<strchr(line,'\n'),
          "requests: the line says the wait for a worker and the wait to be heard");
    request_drop();
}

/* A request's cache mode (SCRIPT_CACHE.md § 5) reaches its job, and one
 * that names none is the default; a module's fetch is said to be one. */
static void request_cache_mode_case(void) {
    fake_fetch_reset();
    loop_page("<script>fetch('/a',{cache:'no-store'});fetch('/b',{cache:'force-cache'});fetch('/c');</script>");
    check(loop_settle() && nrequest_jobs==3,"requests: three fetches are out");
    way_cache_mode_t a=WAY_CACHE_DEFAULT, b=WAY_CACHE_DEFAULT, c=WAY_CACHE_NO_STORE;
    bool module=false;
    for(int i=0;i<nrequest_jobs;i++) {
        const char *url=request_jobs[i].job->url;
        size_t n=strlen(url);
        if(url[n-1]=='a') a=request_jobs[i].job->cache_mode;
        if(url[n-1]=='b') b=request_jobs[i].job->cache_mode;
        if(url[n-1]=='c') c=request_jobs[i].job->cache_mode;
        module|=request_jobs[i].job->module;
    }
    check(a==WAY_CACHE_NO_STORE && b==WAY_CACHE_FORCE_CACHE && c==WAY_CACHE_DEFAULT && !module,
          "requests: each fetch's job carries its cache mode, and none is a module's");
    request_drop();
    fake_fetch_reset();
    loop_page("<script type=module src=/m.js></script>");
    check(loop_settle() && nrequest_jobs==1 && request_jobs[0].job->module &&
          request_jobs[0].job->cache_mode==WAY_CACHE_DEFAULT,
          "requests: a module's fetch is a module's, in the default mode");
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
    check(strstr(record,"request GET http://fixture.test/s.txt: 200 basic, 0 hops, 4 bytes in 0 ms, synchronous")!=NULL,
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
    check(strstr(record,"request GET https://fixture.test/ready.json: 200 basic, 0 hops, 2 bytes in 0 ms\n")!=NULL &&
          strstr(record,"request GET https://fixture.test/sync.json: 200 basic, 0 hops, 2 bytes in 0 ms, synchronous")!=NULL &&
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
    start_trip("https://fixture.test/b",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
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

/* A page at an address longer than its scripts' host keeps
 * (OS64_DOM_URL_MAX) holds only its start there, and a cut address names
 * a page that is not this one: its script fetch, its fetch() and its
 * navigation go with no Referer at all. */
static void request_long_address_case(void) {
    static char address[OS64_DOM_URL_MAX+1024];
    int n=snprintf(address,sizeof(address),"https://fixture.test/long?q=");
    memset(address+n,'a',sizeof(address)-1-n);
    address[sizeof(address)-1]='\0';
    check(sizeof(((way_head_t *)0)->url)>sizeof(address),"long address: the head holds it whole");
    fake_fetch_reset();
    fake_answer("GET","https://fixture.test/f.json",200,"Content-Type: application/json\r\n","{}");
    loop_asked=address; loop_final=address; loop_encrypted=true;
    loop_page("<script>document.addEventListener('DOMContentLoaded',()=>{"
        "var s=document.createElement('script');s.src='/s.js';document.head.appendChild(s);"
        "fetch('/f.json')});setTimeout(()=>{location.href='/next'},10)</script>");
    loop_asked=loop_final=NULL; loop_encrypted=false;
    check(loop_settle() && os64_streq(g.page.way.url,address) && nrequest_jobs==1,
          "long address: the page arrives, and its fetch is out");
    bool script=false;
    for(int i=0;i<nscript_jobs;i++) {
        const yonder_script_job_t *job=script_jobs[i].job;
        if(os64_streq(job->url,"https://fixture.test/s.js")) script=job->hooks.referrer[0]=='\0';
    }
    check(script,"long address: its script's fetch names no Referer");
    char origin[WAY_ORIGIN_TEXT_MAX];
    way_origin_serialize(&request_jobs[0].job->request.origin,origin,sizeof(origin));
    check(request_jobs[0].job->referrer[0]=='\0' && os64_streq(origin,"https://fixture.test"),
          "long address: its fetch() names no Referer, and is still the page's own origin");
    check(request_land("/f.json") && loop_settle(),"long address: the fetch is answered");
    char referer[64]="x";
    fake_field(request_sent("/f.json"),"Referer",referer,sizeof(referer));
    check(referer[0]=='\0',"long address: no Referer reaches the wire");
    now_ms+=20;
    check(script_turn() && nav_sent("https://fixture.test/next",""),
          "long address: its navigation names no Referer");
    request_drop();
    /* The page's address moves the same way: a binding refuses to push an
     * address past its own limit, so a page gets back to its long address
     * by traversing to the entry it arrived at, and away from it by
     * traversing to the one it pushed. Each move is kept or cut as the
     * arrival was: the long entry names nothing, the short one names it. */
    loop_asked=address; loop_final=address; loop_encrypted=true;
    loop_page("<script>onload=()=>history.pushState(null,'','/short');"
        "onpopstate=()=>{var s=document.createElement('script');"
        "s.src=location.pathname=='/short'?'/whole.js':'/cut.js';document.head.appendChild(s)}</script>");
    loop_asked=loop_final=NULL; loop_encrypted=false;
    check(loop_settle() && view_turns() && os64_streq(g.page.way.url,"https://fixture.test/short"),
          "long address: the page pushes a short address");
    session_go(-1,WAY_BY_BROWSER,NULL);
    check(loop_settle() && view_turns() && os64_streq(g.page.way.url,address),"long address: Back to the long one");
    session_go(1,WAY_BY_BROWSER,NULL);
    check(loop_settle() && view_turns() && os64_streq(g.page.way.url,"https://fixture.test/short"),
          "long address: Forward to the short one");
    bool cut=false, whole=false;
    for(int i=0;i<nscript_jobs;i++) {
        const yonder_script_job_t *job=script_jobs[i].job;
        if(os64_streq(job->url,"https://fixture.test/cut.js")) cut=job->hooks.referrer[0]=='\0';
        if(os64_streq(job->url,"https://fixture.test/whole.js"))
            whole=os64_streq(job->hooks.referrer,"https://fixture.test/short");
    }
    check(cut && whole,"long address: a move to the long address names nothing, and back from it names the page");
    request_drop();
}

/* A BODY THAT STREAMS (STREAMS.md § 5, S3): the job writes the body down
 * its pipe a piece at a time, and the window hands the page each piece it
 * asks for while the job is still reading. The fake's read hook is where
 * the window turns, as it would while a worker waits on the wire; each
 * turn notes what the page had then. What the page never asked for is
 * handed over when the job is reaped, and so is the end. */
static char stream_seen[512];
static uint64_t stream_step_ms;
static void stream_hook(size_t at) {
    (void)at;
    now_ms+=stream_step_ms;
    bodies_turn();
    script_rebuild();
    size_t n=strlen(stream_seen);
    snprintf(stream_seen+n,sizeof(stream_seen)-n,"%s%s",n?"|":"",probe_text_of("out"));
}

static void request_stream_case(void) {
    fake_fetch_reset();
    fake_answer("GET","http://fixture.test/s",200,"Content-Type: text/plain\r\n","aaaabbbbcccc")->piece=4;
    loop_page("<p id=out>-</p><script>var log=[];function say(){document.getElementById('out').textContent=log.join(',')}"
        "fetch('/s').then(r=>{log.push('head');say();const rd=r.body.getReader();"
        "const go=()=>rd.read().then(c=>{if(c.done){log.push('done');say();return}"
        "log.push(new TextDecoder().decode(c.value));say();go()});go()});</script>");
    check(loop_settle() && nrequest_jobs==1,"stream: the fetch is out");
    stream_seen[0]='\0';
    stream_step_ms=0;
    fake_read_hook=stream_hook;
    bool landed=request_land("/s");
    fake_read_hook=NULL;
    check(landed && !strcmp(stream_seen,"head|head,aaaa|head,aaaa,bbbb"),
          "stream: the head, then each piece, reach the page while the job reads");
    if(strcmp(stream_seen,"head|head,aaaa|head,aaaa,bbbb")) fprintf(stderr,"stream: seen %s\n",stream_seen);
    check(loop_settle() && script_rebuild() && probe_text_is("out","head,aaaa,bbbb,cccc,done"),
          "stream: the last piece and the end come with the job's reap");
    check(strstr(request_record_text(),"request GET http://fixture.test/s: 200 basic, 0 hops, 12 bytes")!=NULL,
          "stream: the record counts the bytes that streamed");
    check(s_streamed==NULL,"stream: the window holds no pipe once the request is done");
    request_drop();

    /* Nobody reads while the job runs: the whole body is handed over at the
     * reap, queued, and read whole later. */
    fake_fetch_reset();
    fake_answer("GET","http://fixture.test/q",200,"","0123456789")->piece=3;
    loop_page("<p id=out>-</p><script>fetch('/q').then(r=>{window.held=r});"
        "setTimeout(()=>held.text().then(t=>{document.getElementById('out').textContent=t}),50);</script>");
    check(loop_settle() && request_land("/q") && loop_settle(),"stream: a body nobody reads yet");
    now_ms+=60;
    check(script_turn() && script_rebuild() && probe_text_is("out","0123456789"),
          "stream: what waited is read whole later");
    request_drop();

    /* A body that breaks part way: the page's read rejects, and the record
     * says why. */
    fake_fetch_reset();
    fake_answer_t *broke=fake_answer("GET","http://fixture.test/b",200,"","abcdefghij");
    broke->piece=4;
    broke->cut=6;
    loop_page("<p id=out>-</p><script>fetch('/b').then(r=>r.text()).then(t=>{document.getElementById('out').textContent=t},"
        "e=>{document.getElementById('out').textContent=e.name})</script>");
    check(loop_settle() && request_land("/b") && loop_settle() && script_rebuild() && probe_text_is("out","TypeError"),
          "stream: a body that breaks part way rejects the page's read");
    check(strstr(request_record_text(),"FAILED request: GET http://fixture.test/b: ")!=NULL,
          "stream: the record has the broken request, as a FAILED line");
    request_drop();

    /* The page cancels part way: the job stops reading at its next piece. */
    fake_fetch_reset();
    static char big[4096];
    memset(big,'x',sizeof(big)-1);
    fake_answer("GET","http://fixture.test/c",200,"",big)->piece=16;
    loop_page("<script>fetch('/c').then(r=>{const rd=r.body.getReader();rd.read().then(()=>rd.cancel())});</script>");
    stream_seen[0]='\0';
    fake_read_hook=stream_hook;
    check(loop_settle() && request_land("/c"),"stream: a body the page cancels");
    fake_read_hook=NULL;
    check(fake_reads<8 && s_streamed==NULL,"stream: the job stopped reading once the page cancelled");
    request_drop();

    /* What a reaped job's pipe still holds goes a piece a turn, never all
     * in one turn: the window paints between them. */
    fake_fetch_reset();
    static char three[3 * 1024 * 1024 + 512];
    memset(three,'z',sizeof(three)-1);
    fake_answer("GET","http://fixture.test/p",200,"",three);
    loop_page("<p id=out>-</p><script>var got=0;fetch('/p').then(r=>{const rd=r.body.getReader();"
        "const go=()=>rd.read().then(c=>{if(c.done){document.getElementById('out').textContent='done '+got;return}"
        "got+=c.value.length;document.getElementById('out').textContent=String(got);go()});go()});</script>");
    check(loop_settle() && request_land("/p"),"stream: a body still in its pipe when its job is reaped");
    char paced[160]="";
    for(int t=0;t<4;t++) {
        bodies_turn();
        script_rebuild();
        size_t n=strlen(paced);
        snprintf(paced+n,sizeof(paced)-n,"%s%s",n?"|":"",probe_text_of("out"));
    }
    check(!strcmp(paced,"-|1048576|2097152|3145728"),"stream: the head, then a piece a turn");
    if(strcmp(paced,"-|1048576|2097152|3145728")) fprintf(stderr,"stream: paced %s\n",paced);
    check(loop_settle() && script_rebuild() && probe_text_is("out","done 3146239") && s_streamed==NULL,
          "stream: then the rest and the end");
    request_drop();

    /* XMLHttpRequest: LOADING and progress as the pieces come, at most once
     * in 50 ms; the rest, and the end, at the reap. */
    fake_fetch_reset();
    fake_answer("GET","http://fixture.test/x",200,"Content-Type: text/plain\r\n","aaaabbbbcccc")->piece=4;
    loop_page("<p id=out>-</p><script>var log=[];var x=new XMLHttpRequest();x.open('GET','/x');"
        "x.onreadystatechange=()=>{log.push(x.readyState+':'+x.responseText.length);"
        "document.getElementById('out').textContent=log.join(',')};x.send();</script>");
    stream_seen[0]='\0';
    stream_step_ms=60;
    fake_read_hook=stream_hook;
    check(loop_settle() && request_land("/x"),"stream: an XMLHttpRequest whose body streams");
    fake_read_hook=NULL;
    stream_step_ms=0;
    check(!strcmp(stream_seen,"2:0|2:0,3:4|2:0,3:4,3:8"),"stream: LOADING as each piece comes");
    if(strcmp(stream_seen,"2:0|2:0,3:4|2:0,3:4,3:8")) fprintf(stderr,"stream: xhr seen %s\n",stream_seen);
    check(loop_settle() && script_rebuild() && probe_text_is("out","2:0,3:4,3:8,4:12"),
          "stream: the last piece inside the throttle, then DONE");
    request_drop();
}

static void request_cases(void) {
    request_long_address_case();
    navigation_cases();
    request_redirected_page_case();
    request_same_origin_case();
    request_cache_mode_case();
    request_waits_case();
    request_cors_case();
    request_xhr_case();
    request_cancel_case();
    request_stream_case();
}
