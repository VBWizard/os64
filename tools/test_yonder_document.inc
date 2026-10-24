/* P1-DA END TO END (MODERN_WEB.md § P1-Da, as built): what a page reads of
 * its own document through yonder's loop: readyState's three values over
 * its life with readystatechange before DOMContentLoaded and load, the
 * referrer a navigation leaves it, currentScript around each script,
 * activeElement after a focus() the window carried out, title's setter
 * where yonder reads a page's title, and a banking SDK's CustomEvent
 * idiom. */

/* probe_text_is, saying what the element holds when it is not `text`. */
static bool document_out_is(const char *text) {
    if(probe_text_is("out",text)) return true;
    os64_html_node_t *n=probe_id("out");
    fprintf(stderr,"document: #out holds \"%s\", not \"%s\"\n",
        n!=NULL && n->first_child!=NULL && n->first_child->kind==OS64_HTML_TEXT ? n->first_child->text : "(no text)",
        text);
    return false;
}

/* The page the stream is fetching, answered: its head (at `final`, or the
 * fixture's address), its body and its verdict. */
static void document_answer(const char *final, const char *rest) {
    char *html=pad_page(rest);
    way_head_t h=stream_head("text/html","",false);
    if(final!=NULL) os64_strcopy(h.url,sizeof(h.url),final);
    os64_strcopy(h.last_modified,sizeof(h.last_modified),"Tue, 14 Nov 2023 22:13:20 GMT");
    yonder_mail_post_head(g.stream.mail,&h);
    stream_post_body((const uint8_t *)html,strlen(html),YONDER_STREAM_CHUNK);
    stream_post_verdict(true,OS64_FETCH_OK,"");
    free(html);
}

static void document_ready_case(void) {
    loop_page("<p id=out>x</p><script>var seen=[document.readyState];"
        "document.onreadystatechange=function(){seen.push('rsc:'+document.readyState)};"
        "document.addEventListener('DOMContentLoaded',function(){seen.push('dcl:'+document.readyState)});"
        "addEventListener('load',function(){seen.push('load:'+document.readyState);"
        "document.getElementById('out').textContent=seen.join('|')});</script>"
        "<script defer src=late.js></script>");
    check(loop_settle(),"document: the parse runs to the deferred script");
    check(script_land("/late.js","seen.push('defer:'+document.readyState)"),"document: the deferred script lands");
    check(loop_settle() && script_rebuild() &&
        document_out_is("loading|defer:loading|rsc:interactive|dcl:interactive|rsc:complete|load:complete"),
        "document: loading, interactive at DOMContentLoaded and complete at load, readystatechange before each");
    loop_drop();
    /* A page that runs its first script after its load is born complete. */
    loop_page("<p id=out onclick=\"this.textContent=document.readyState\">x</p>");
    check(loop_settle() && g.page.tree!=NULL,"document: a page with no script arrives");
    const os64_html_node_t *out=probe_id("out");
    os64_dom_event_t click={.type="click",.bubbles=true};
    os64_js_outcome_t outcome;
    yonder_scripts_dispatch(g.page.scripts,out,&click,NULL,&outcome);
    check(outcome.status==OS64_JS_OK && script_rebuild() && document_out_is("complete"),
        "document: a binding made at the first click after the load reads complete");
    loop_drop();
}

static void document_referrer_case(void) {
    /* A typed address names no page. */
    loop_page("<p id=out>x</p><script>document.getElementById('out').textContent='['+document.referrer+']'</script>");
    check(loop_settle() && script_rebuild() && document_out_is("[]"),"document: an address typed has no referrer");
    loop_drop();
    /* A script's navigation: the next page's referrer is the page that
     * asked, as its Referer was; across origins, that page's origin. */
    static const struct { const char *final, *want, *what; } kTrips[] = {
        {NULL, "[http://fixture.test/final]", "document: a navigation's referrer is the page that asked"},
        {"http://other.test/next", "[http://fixture.test/]",
         "document: a navigation to another origin is referred by the asking page's origin"},
    };
    for(size_t i=0;i<sizeof(kTrips)/sizeof(kTrips[0]);i++) {
        loop_page("<script>location.href='next.html'</script>");
        loop_settle();
        yonder_trip_t *trip=pool_work.job;
        check(g.stream.active && trip!=NULL && os64_streq(trip->referrer,"http://fixture.test/final"),
            "document: the navigation's trip names the asking page");
        document_answer(kTrips[i].final,
            "<p id=out>x</p><script>document.getElementById('out').textContent='['+document.referrer+']'</script>");
        check(loop_settle() && script_rebuild() && document_out_is(kTrips[i].want),kTrips[i].what);
        loop_drop();
    }
    /* A Reload, by the button or by location.reload(), keeps the page's own
     * referrer, though its request names nothing. */
    for(int how=0;how<2;how++) {
        loop_page("<script>location.href='next.html'</script>");
        loop_settle();
        document_answer("http://other.test/next",
            "<p id=out>x</p><script>document.getElementById('out').textContent='['+document.referrer+']';"
            "setTimeout(function(){location.reload()},10)</script>");
        check(loop_settle() && script_rebuild() && document_out_is("[http://fixture.test/]"),
            "document: the page to be reloaded has its referrer");
        if(how==0) click_reload(NULL,NULL);
        else { now_ms=200; script_turn(); }
        yonder_trip_t *trip=pool_work.job;
        check(g.stream.active && trip!=NULL && trip->referrer[0]=='\0',"document: a Reload's request names nothing");
        document_answer("http://other.test/next",
            "<p id=out>x</p><script>document.getElementById('out').textContent='['+document.referrer+']'</script>");
        check(loop_settle() && script_rebuild() && document_out_is("[http://fixture.test/]"),
            how==0 ? "document: the Reload button keeps the page's referrer"
                   : "document: location.reload() keeps the page's referrer");
        loop_drop();
    }
}

static void document_current_script_case(void) {
    loop_page("<p id=out>x</p><script id=first>var seen=[document.currentScript.id];"
        "setTimeout(function(){seen.push(String(document.currentScript));"
        "document.getElementById('out').textContent=seen.join('|')},10)</script>"
        "<script id=second>seen.push(document.currentScript.id);"
        "var s=document.createElement('script');s.id='made';s.textContent='seen.push(document.currentScript.id)';"
        "document.body.appendChild(s)</script>");
    check(loop_settle() && g.page.tree!=NULL,"document: the page arrives");
    now_ms=200;
    check(script_turn() && document_out_is("first|second|made|null"),
        "document: currentScript is each script as it runs, and null in a timer");
    loop_drop();
}

static void document_focus_case(void) {
    loop_page("<p id=out>x</p><input id=field><script>"
        "var seen=[document.activeElement===document.body,document.hasFocus()];"
        "addEventListener('load',function(){document.getElementById('field').focus();"
        "seen.push(document.activeElement===document.body)});"
        "setTimeout(function(){seen.push(document.activeElement.id,document.hasFocus());"
        "document.getElementById('out').textContent=seen.join('|')},10)</script>");
    check(loop_settle() && g.page.tree!=NULL,"document: the page arrives");
    /* The window's next turn: the focus its widgets took is followed. */
    inputs_run();
    now_ms=200;
    check(script_turn() && document_out_is("true|false|true|field|true"),
        "document: the body before anything has focus, and no window focus while the page is not shown; "
        "the focused input once the window carried out the focus() a script asked for");
    loop_drop();
}

static void document_title_case(void) {
    loop_page("<title>first</title><p id=out>x</p><script>document.title='  set  by\\tthe script ';"
        "document.getElementById('out').textContent=document.title</script>");
    check(loop_settle() && script_rebuild() && document_out_is("set by the script"),
        "document: title's setter, read back");
    char named[120];
    title_of(page_doc(&g.page),named,sizeof(named));
    check(os64_streq(named,"set by the script"),"document: the title yonder reads of the page is the one the script set");
    loop_drop();
}

static void document_custom_event_case(void) {
    /* A banking SDK's common.js died at load on `'initCustomEvent' is not
     * a function`: its idiom, whole. */
    loop_page("<p id=out>x</p><div id=el></div><script>var el=document.getElementById('el'),got='none';"
        "el.addEventListener('x',function(e){got=e.detail.a+':'+e.bubbles+':'+e.cancelable});"
        "var e=document.createEvent('CustomEvent');e.initCustomEvent('x',true,true,{a:1});el.dispatchEvent(e);"
        "document.getElementById('out').textContent=got+':'+(new CustomEvent('y',{detail:2}).detail)</script>");
    check(loop_settle() && script_rebuild() && document_out_is("1:true:true:2"),
        "document: a banking SDK's CustomEvent idiom works");
    check(strstr(window_record_text(),"FAILED")==NULL,"document: and the page's record has no FAILED line");
    loop_drop();
}

static void document_cases(void) {
    document_ready_case();
    document_referrer_case();
    document_current_script_case();
    document_focus_case();
    document_title_case();
    document_custom_event_case();
}
