/* P1-W END TO END (MODERN_WEB.md § P1-W, as built): crypto through
 * yonder's entropy (scripts.c's hook over libtls's door, faked by this
 * host as fake_entropy_*), and a queueMicrotask callback that throws,
 * reported on the page's record while the page carries on. And P2a's
 * (§ P2a, as built): rejections nothing handled on the record, and the
 * window's error and rejection events. */

static const char *window_record_text(void) {
    static char text[16384];
    text[0]='\0';
    if(g.page.diag!=NULL) yonder_diag_render(g.page.diag,text,sizeof(text));
    return text;
}

/* Whether the page's record holds `line`; the record is shown when not. */
static bool window_record_has(const char *line) {
    const char *text=window_record_text();
    if(strstr(text,line)!=NULL) return true;
    fprintf(stderr,"window: the record has no \"%s\":\n%s\n",line,text);
    return false;
}

static void window_crypto_case(void) {
    fake_entropy_fail=false;
    loop_page("<p id=out>x</p><script>var a=crypto.getRandomValues(new Uint8Array(8));"
        "document.getElementById('out').textContent=[a.length,a.some(x=>x!==0),isSecureContext,"
        "'randomUUID' in crypto,origin].join(':');</script>");
    check(loop_settle() && script_rebuild() && probe_text_is("out","8:true:false:false:http://fixture.test"),
        "window: crypto fills from yonder's entropy; an http page is no secure context");
    loop_drop();
    fake_entropy_fail=true;
    loop_page("<p id=out>x</p><script>try{crypto.getRandomValues(new Uint8Array(4))}"
        "catch(e){document.getElementById('out').textContent=e.name}</script>"
        "<script>crypto.getRandomValues(new Uint8Array(1))</script>");
    check(loop_settle() && script_rebuild() && probe_text_is("out","OperationError"),
        "window: entropy yonder cannot read is an OperationError the page can catch");
    check(window_record_has("FAILED script http://fixture.test/final#inline-2: OperationError"),
        "window: and one it does not catch is its script's FAILED line");
    fake_entropy_fail=false;
    loop_drop();
}

static void window_microtask_case(void) {
    loop_page("<p id=out>x</p><script>queueMicrotask(()=>{throw new Error('in a microtask')});"
        "queueMicrotask(()=>{document.getElementById('out').textContent='after'});</script>"
        "<script>document.getElementById('out').title='next script'</script>");
    check(loop_settle() && script_rebuild() && probe_text_is("out","after"),
        "window: the microtask after one that threw still runs");
    check(window_record_has("FAILED script http://fixture.test/final#inline-1: Error: in a microtask"),
        "window: a microtask's throw is reported on the page's record, against the script that queued it");
    check(strstr(window_record_text(),"FAILED script http://fixture.test/final#inline-2")==NULL &&
          strstr(window_record_text(),"verdict: 1 FAILED\n")!=NULL,
        "window: and the next script runs clean");
    loop_drop();
}

/* P2a END TO END (MODERN_WEB.md § P2a, as built): the page's record says a
 * rejection nothing handled, by the script, event or timer whose task left
 * it, and the window's events reach the page's handlers. */
static void window_rejection_case(void) {
    loop_page("<p id=b>button</p><p id=log>-</p><script>var log=[];"
        "function note(s){log.push(s);document.getElementById('log').textContent=log.join('|')}"
        "onunhandledrejection=function(e){note('unhandled '+e.reason.message)};"
        "onrejectionhandled=function(e){note('handled '+e.reason.message)};"
        "var kept=Promise.reject(new Error('from the script'));Promise.reject(new Error('and another'));"
        "Promise.reject(new Error('caught')).catch(function(){});"
        "document.getElementById('b').addEventListener('click',async function(){throw new TypeError('in a click')});"
        "async function later(){await new Promise(function(r){setTimeout(r,10)});throw new Error('after a timer')}"
        "later();setTimeout(function(){kept.catch(function(){})},50);</script>");
    check(loop_settle() && script_rebuild(),"window: a page that leaves rejections unhandled arrives");
    check(window_record_has("FAILED script http://fixture.test/final#inline-1: unhandled rejection: "
                            "Error: from the script (1)"),
        "window: a script's unhandled rejection is its FAILED line, said as one");
    check(window_record_has("FAILED script http://fixture.test/final#inline-1: unhandled rejection: "
                            "Error: and another (1)"),
        "window: and each rejection it left is a line of its own");
    check(strstr(window_record_text(),"caught")==NULL,"window: a caught rejection is no failure");
    input_click_at("b");
    check(window_record_has("FAILED script click event: unhandled rejection: TypeError: in a click (1)"),
        "window: an async click handler that throws is the click's FAILED line");
    check(status_says("unhandled rejection: TypeError: in a click"),
        "window: and the status line says it, as it says a script's error");
    now_ms+=100;
    for(int i=0;i<4;i++) script_turn();
    check(window_record_has("FAILED script http://fixture.test/final#timer-1: unhandled rejection: "
                            "Error: after a timer (1)"),
        "window: one a timer's task left is the timer's");
    check(script_rebuild() && probe_text_is("log","unhandled from the script|unhandled and another|"
                                                 "unhandled in a click|unhandled after a timer|handled from the script"),
        "window: each was the page's unhandledrejection, and the late handler its rejectionhandled");
    loop_drop();
    loop_page("<p id=log>-</p><script>onunhandledrejection=function(e){e.preventDefault()};"
        "onerror=function(m,f,l,c){document.getElementById('log').textContent=[m,f,l,c].join(' ')};"
        "Promise.reject(new Error('handled by the page'))</script><script>missing()</script>");
    check(loop_settle() && script_rebuild() && strstr(window_record_text(),"handled by the page")==NULL,
        "window: a rejection the page's listener cancelled is no failure");
    check(probe_text_is("log","Uncaught ReferenceError: 'missing' is not defined http://fixture.test/final 1 1") &&
          window_record_has("FAILED script http://fixture.test/final#inline-2: ReferenceError: 'missing' is not "
                            "defined (1)"),
        "window: window.onerror hears a script's error, which is its FAILED line as before");
    loop_drop();
}

/* A flood of rejections in one task costs the record seventeen lines past
 * the task's first failure, the last counting the rest, and the record's
 * lookups stay a few probes a line (diag.c's index). */
static void window_rejection_flood_case(void) {
    loop_page("<script>for(var i=0;i<1000;i++)Promise.reject(i)</script>");
    uint64_t before=yonder_diag_steps();
    check(loop_settle(),"window: a page that leaves a thousand rejections arrives");
    check(window_record_has("FAILED script http://fixture.test/final#inline-1: unhandled rejection: 0 (1)") &&
          window_record_has("FAILED script http://fixture.test/final#inline-1: unhandled rejection: 16 (1)") &&
          strstr(window_record_text(),"unhandled rejection: 17 (1)")==NULL &&
          window_record_has("FAILED script http://fixture.test/final#inline-1: and 983 more unhandled "
                            "rejections (1)"),
        "window: the first, sixteen more, and one line counting the rest");
    check(yonder_diag_steps()-before<2000,"window: and the record found its lines in a few probes each");
    loop_drop();
}

/* A script is muted, and named, by where it came from: a same-origin
 * address that redirects to another site is that site's script; one that
 * redirects within the site is the page's, told whole. */
static void window_redirect_case(const char *final, bool muted) {
    loop_page("<p id=log>-</p><script>var log=[];"
        "onerror=function(m,f){log.push(m+' '+f);document.getElementById('log').textContent=log.join('|')};"
        "onunhandledrejection=function(e){log.push('unhandled '+e.reason.message);"
        "document.getElementById('log').textContent=log.join('|')}</script>"
        "<script src=/lib.js></script>");
    char record[160], told[200];
    check(loop_settle() && nscript_jobs==1,"window: the parse waits for the script's fetch");
    check(script_land_from("/lib.js","Promise.reject(new Error('rejected'));throw new Error('thrown')",final),
        "window: the redirected script's fetch comes back");
    check(loop_settle() && script_rebuild(),"window: the page with a redirected script settles");
    snprintf(record,sizeof(record),"FAILED script %s: Error: thrown (1)",final);
    snprintf(told,sizeof(told),"Uncaught Error: thrown %s|unhandled rejected",final);
    check(window_record_has(record),"window: the record names the script by the address it came from");
    check(probe_text_is("log",muted ? "Script error. " : told),
        muted ? "window: a same-origin address redirected to another site is muted"
              : "window: one redirected within the site is told whole, by where it came from");
    loop_drop();
}

static void window_cases(void) {
    window_redirect_case("http://other.test/lib.js",true);
    window_redirect_case("http://fixture.test/moved/lib.js",false);
    window_rejection_case();
    window_rejection_flood_case();
    window_crypto_case();
    window_microtask_case();
}
