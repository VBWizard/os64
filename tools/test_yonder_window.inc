/* P1-W END TO END (MODERN_WEB.md § P1-W, as built): crypto through
 * yonder's entropy (scripts.c's hook over libtls's door, faked by this
 * host as fake_entropy_*), and a queueMicrotask callback that throws,
 * reported on the page's record while the page carries on. */

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

static void window_cases(void) {
    window_crypto_case();
    window_microtask_case();
}
