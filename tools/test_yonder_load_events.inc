/* LOAD AND ERROR AT LINK, STYLE AND SCRIPT ELEMENTS (LOAD_EVENTS.md), end to
 * end through yonder's window: a sheet's verdict is owed to its element and
 * fired as a task of its own; a script's event is part of the script's own
 * task. Sheets land by sheet_land, classic scripts by script_land, modules
 * by request_land, as the worker would answer each. */

/* The loop's turns, then the shown page's own (script_turn), which is
 * where an event owed after the page arrived is fired, until neither has
 * anything to do; then the page laid out again as the tree now stands. */
static bool load_settle(void) {
    for(int round=0;round<50;round++) {
        if(!loop_settle()) return false;
        bool ran=false;
        for(int i=0;i<200 && script_turn();i++) ran=true;
        if(g.page.tree!=NULL) (void)script_rebuild();
        if(!ran) return true;
    }
    return false;
}

/* A page whose capture listeners on document write each element's load and
 * error into #out, in order; and one on window, which DOM's "get the
 * parent" keeps every element's load from (it hears their error). */
#define LOAD_LOG "<p id=out></p><script>var log=[],after=0;function note(n){log.push(n);" \
    "document.getElementById('out').textContent=log.join('|')}" \
    "function who(e){return e.target.tagName+(e.target.id?' '+e.target.id:'')}" \
    "document.addEventListener('load',e=>{if(e.target.tagName)note('load '+who(e))},true);" \
    "document.addEventListener('error',e=>{if(e.target.tagName)note('error '+who(e))},true);" \
    "window.addEventListener('load',e=>{if(e.target!==document)note('window heard load '+who(e))},true);" \
    "window.addEventListener('error',e=>{if(e.target.tagName)note('window heard error '+who(e))},true);</script>"

/* The shape that drew a social network's logged-out page blank: an inline
 * head script listens on document in capture, the parser reaches the
 * stylesheet's link after it, and the listener's insertion of the page's
 * entry module is what runs the page. */
static void load_loader_case(void) {
    for(int failing=0;failing<2;failing++) {
        loop_page("<p id=out></p><script>var log=[];function note(n){log.push(n);"
            "document.getElementById('out').textContent=log.join('|')}"
            "function onSheet(e){var t=e.target;if(!t||t.tagName!=='LINK')return;"
            "note(e.type+' '+e.bubbles+' '+e.cancelable+' '+e.isTrusted);"
            "document.removeEventListener('load',onSheet,true);document.removeEventListener('error',onSheet,true);"
            "var s=document.createElement('script');s.type='module';s.textContent=\"note('entry')\";"
            "document.head.appendChild(s)}"
            "document.addEventListener('load',onSheet,true);document.addEventListener('error',onSheet,true);"
            "</script><link rel=stylesheet href=styles.css><p>shell</p>");
        check(load_settle() && nsheet_jobs==1,"load events: the loader's sheet is sent for");
        check(sheet_land("/styles.css",failing ? NULL : "p{color:red}") && load_settle() && g.page.tree!=NULL &&
              script_rebuild() && probe_text_is("out",failing ? "error false false true|entry" : "load false false true|entry"),
              failing ? "load events: a sheet that failed fires error at its link, not bubbling, not cancelable, "
                        "trusted, and a capture listener on document hears it and injects the entry module"
                      : "load events: a sheet that came fires load at its link, not bubbling, not cancelable, "
                        "trusted, and a capture listener on document hears it and injects the entry module");
        loop_drop();
    }
}

/* `<link media=print onload="this.media='all'">`, on a page with no other
 * script: the handler attribute makes the runtime when its load comes. */
static void load_handler_only_case(void) {
    loop_page("<link id=l rel=stylesheet media=print href=late.css onload=\"this.media='all'\"><p>text</p>");
    check(load_settle() && nsheet_jobs==1 && attr_is(probe_id("l"),"media","print"),
          "load events: a print sheet is fetched all the same");
    check(sheet_land("/late.css","p{color:red}") && load_settle() && attr_is(probe_id("l"),"media","all"),
          "load events: its onload runs on a page with no other script, and flips its media");
    loop_drop();
}

/* A style fires load, a link yonder will not fetch fires error, a style a
 * script inserts fires after that script, never inside it; window hears an
 * element's error but never its load. Across a rebuild (a timer changes
 * the tree) nothing is told twice: not the reused parses, not the failed
 * sheet fetched again and failing again. An href changed is a new fetch,
 * and its element is told again; so is a style whose text changed. A style
 * a script inserts and then measures, which the measuring page parses
 * inside that script, is told once, by its own page. */
static void load_sheets_case(void) {
    loop_page(LOAD_LOG "<style id=s1>p{}</style><link id=d rel=stylesheet href='data:text/css,p{}'>"
        "<link id=a rel=stylesheet href=a.css><link id=gone rel=stylesheet href=gone.css>"
        "<script>var st=document.createElement('style');st.id='s2';st.textContent='b{}';"
        "document.head.appendChild(st);after=1;"
        "document.addEventListener('load',e=>{if(e.target.id==='s2')note('s2 after '+after)},true);"
        "setTimeout(()=>{document.body.appendChild(document.createElement('div'));note('timer')},1000);"
        "setTimeout(()=>{document.getElementById('a').href='b.css'},2000);"
        "setTimeout(()=>{var m=document.createElement('style');m.id='s3';m.textContent='#out{width:10px}';"
        "document.head.appendChild(m);note('measured '+(document.getElementById('out').offsetWidth>0));"
        "document.getElementById('s1').textContent='p{color:green}'},3000);</script>");
    check(load_settle() && nsheet_jobs==2,"load events: the two fetchable sheets are sent for");
    check(sheet_land("/a.css","p{color:blue}") && sheet_land("/gone.css",NULL) && load_settle() &&
          g.page.tree!=NULL && script_rebuild() && load_settle(),"load events: the sheets' page arrives");
    const char *out=element_text("out");
    check(strcmp(out,"load STYLE s1|window heard error LINK d|error LINK d|load LINK a|"
                     "window heard error LINK gone|error LINK gone|load STYLE s2|s2 after 1")==0,
          "load events: each style's load, each link's load or error once; the inserted style's after its script; "
          "a data: sheet, which yonder does not fetch, fails; window hears errors, never an element's load");
    /* A rebuild, made by the page's timer changing the tree. */
    char before[1024];
    os64_strcopy(before,sizeof(before),out);
    now_ms+=1000;
    check(load_settle() && script_rebuild() && load_settle(),"load events: a timer changes the tree, and it is laid out again");
    /* The failed sheet was not reused: it is fetched again, and fails again. */
    check(nsheet_jobs==1 && sheet_land("/gone.css",NULL) && load_settle(),"load events: the failed sheet is fetched again");
    out=element_text("out");
    char expect[1100];
    snprintf(expect,sizeof(expect),"%s|timer",before);
    check(strcmp(out,expect)==0,"load events: across a rebuild no element is told twice, the failed sheet included");
    now_ms+=1000;
    check(load_settle() && script_rebuild() && load_settle() && sheet_land("/b.css","p{}") && load_settle() &&
          count_in(element_text("out"),"load LINK a")==2,"load events: an href changed is told again");
    now_ms+=1000;
    check(load_settle() && script_rebuild() && load_settle(),"load events: a timer inserts and measures a style");
    out=element_text("out");
    check(count_in(out,"measured true")==1 && count_in(out,"load STYLE s3")==1,
          "load events: a style a script measured is told once, after the script, by its own page");
    check(count_in(out,"load STYLE s1")==2,"load events: a style whose text changed is told again");
    loop_drop();
}

/* A script from a file fires load after it ran, in its own task (before the
 * next script runs), whether or not it threw; an inline script never fires;
 * a fetch that failed and an empty src fire error, in their place. */
static void load_scripts_case(void) {
    loop_page("<p id=out></p><script>var log=[];function note(n){log.push(n);"
        "document.getElementById('out').textContent=log.join('|')}</script>"
        "<script src=one.js onload=\"note('load one')\"></script>"
        "<script onload=\"note('inline load')\">note('inline')</script>"
        "<script src=two.js onload=\"note('load two')\" onerror=\"note('error two')\"></script>"
        "<script src=gone.js onload=\"note('load gone')\" onerror=\"note('error gone')\"></script>"
        "<script src='' onload=\"note('load empty')\" onerror=\"note('error empty')\"></script>"
        "<script>note('end')</script>");
    check(load_settle() && script_land("/one.js","note('one')") && load_settle() &&
          script_land("/two.js","note('two');throw Error('thrown')") && load_settle() &&
          script_land("/gone.js",NULL) && load_settle() && g.page.tree!=NULL && script_rebuild(),
          "load events: the scripts' page arrives");
    check(probe_text_is("out","one|load one|inline|two|load two|error gone|error empty|end"),
          "load events: each file's load after it ran and before the next, a throw's too; no inline load; "
          "error for a failed fetch and an empty src, in their place");
    loop_drop();
}

/* A module from a file fires load after it ran, a syntax error's too (HTML
 * rethrows it: the module ran); a module whose graph failed fires error,
 * an inline one's as well. */
static void load_modules_case(void) {
    fake_fetch_reset();
    fake_answer("GET","http://fixture.test/m/ok.js",200,MODULE_JS,"note('ok')");
    fake_answer("GET","http://fixture.test/m/bad.js",200,MODULE_JS,"note(;");
    fake_answer("GET","http://fixture.test/m/a.js",200,MODULE_JS,"import './missing.js';note('never')");
    fake_answer("GET","http://fixture.test/m/missing.js",404,"","");
    module_page("<script type=module src=/m/ok.js onload=\"note('load ok')\"></script>"
        "<script type=module src=/m/bad.js onload=\"note('load bad')\" onerror=\"note('error bad')\"></script>"
        "<script type=module src=/m/a.js onload=\"note('load a')\" onerror=\"note('error a')\"></script>"
        "<script type=module onload=\"note('load inline')\" onerror=\"note('error inline')\">"
        "import '/m/missing.js'</script>");
    check(load_settle() && request_land("/m/ok.js") && request_land("/m/bad.js") && request_land("/m/a.js") &&
          request_land("/m/missing.js") && load_settle() && g.page.tree!=NULL && script_rebuild(),
          "load events: the modules' page arrives");
    check(probe_text_is("out","ok|load ok|load bad|error a|error inline"),
          "load events: a module's load after it ran, a syntax error's too; error for a failed graph, inline or not");
    request_drop();
}

/* Fable's finding 1: one script inserts 200 styles on a page with a window
 * load listener. Each owes a load nothing on its path listens for, so the
 * drain passes them all over without a turn each, and the page's timer is
 * not kept waiting behind them. */
static void load_many_styles_case(void) {
    loop_page("<p id=out></p><script>addEventListener('load',()=>{});"
        "for(var i=0;i<200;i++){var s=document.createElement('style');s.textContent='i'+i+'{}';document.head.appendChild(s)}"
        "setTimeout(()=>{document.getElementById('out').textContent='timer'},0)</script>");
    now_ms+=50;
    int turns=0;
    for(;turns<200 && !(g.page.tree!=NULL && yonder_scripts_owed_pending(g.page.scripts)==false &&
                        probe_text_is("out","timer"));turns++) {
        (void)stream_turn(); (void)coming_turn(); (void)bodies_turn(); (void)script_turn();
        if(g.page.tree!=NULL) (void)script_rebuild();
    }
    check(turns<20 && !yonder_scripts_owed_pending(g.page.scripts) && probe_text_is("out","timer"),
          "load events: 200 owed loads nobody hears are passed over in a handful of turns, and the timer runs");
    loop_drop();
}

/* The cold read's first finding: a sheet that settles while the page is
 * still streaming, before anything has made its scripts, is told when they
 * are made: by the first script the parser stops at, or by the finished
 * page for its handler attributes. Its onload runs either way. */
static void load_before_scripts_case(void) {
    for(int scripted=0;scripted<2;scripted++) {
        /* The page arrives in two parts, the sheet landing between them. */
        stream_window();
        g.scripts_on=true; g.script_ms=5000; now_ms=100; g.modern=true;
        start_trip("http://fixture.test/p",NULL,NULL,NAV_GO,WAY_BY_BROWSER,NULL);
        way_head_t h=stream_head("text/html","",false);
        yonder_mail_post_head(g.stream.mail,&h);
        char *first=pad_page("<link id=l rel=stylesheet media=print href=early.css onload=\"this.media='all'\"><p>a</p>");
        stream_post_body((const uint8_t *)first,strlen(first),YONDER_STREAM_CHUNK);
        free(first);
        for(int turns=0;nsheet_jobs==0 && turns<10;turns++) (void)stream_turn();
        check(nsheet_jobs==1 && g.stream.active && g.stream.scripts==NULL,
              "load events: the sheet is out while the page streams, before it has scripts");
        check(sheet_land("/early.css","p{color:red}") && g.stream.active && g.stream.scripts==NULL,
              "load events: the sheet lands before the page has scripts");
        const char *rest=scripted ? "<p>b</p><script>var late=1</script><p>c</p>" : "<p>b</p>";
        stream_post_body((const uint8_t *)rest,strlen(rest),YONDER_STREAM_CHUNK);
        stream_post_verdict(true,OS64_FETCH_OK,"");
        check(load_settle() && attr_is(probe_id("l"),"media","all"),
              scripted ? "load events: a sheet settled before the first script is told when that script makes them"
                       : "load events: a sheet settled before a script-less page finished is told when the page makes "
                         "its scripts for the handler");
        loop_drop();
    }
}

/* yonder_scripts_owe's one allocation refused: nothing kept and nothing
 * held, and the next owe is kept. Then the ring grows past its first
 * size, and is compacted when a drain stopped part way (a heard event in
 * the middle) and it fills: every event still fires, in order, once. */
static void load_owe_ring_case(void) {
    probe_page("<p id=out></p><style id=heard onload=\"var o=document.getElementById('out');"
               "o.textContent=o.textContent+'h'\"></style><style id=quiet></style>",true);
    yonder_scripts_t *s=g.page.scripts;
    os64_html_node_t *heard=probe_id("heard"), *quiet=probe_id("quiet");
    size_t was=live;
    fail_at=attempts+1;
    check(!yonder_scripts_owe(s,quiet,false) && !yonder_scripts_owed_pending(s) && live==was,
          "load events: an owe refused for want of memory keeps nothing");
    fail_at=0;
    check(yonder_scripts_owe(s,quiet,false) && yonder_scripts_owed_pending(s),"load events: the next owe is kept");
    for(int i=0;i<4;i++) (void)yonder_scripts_owe(s,quiet,false);
    (void)yonder_scripts_owe(s,heard,false);
    for(int i=0;i<10;i++) (void)yonder_scripts_owe(s,quiet,false);
    os64_js_outcome_t out;
    check(yonder_scripts_owed_fire(s,&out) && out.status==OS64_JS_OK && probe_text_is("out","h") &&
          yonder_scripts_owed_pending(s),"load events: a drain stops at the first heard event, the rest still owed");
    for(int i=0;i<3;i++) (void)yonder_scripts_owe(s,quiet,false);
    (void)yonder_scripts_owe(s,heard,false);
    for(int i=0;i<30;i++) (void)yonder_scripts_owe(s,quiet,false);
    check(yonder_scripts_owed_fire(s,&out) && probe_text_is("out","hh") && yonder_scripts_owed_pending(s),
          "load events: compacted and grown, the ring keeps its order: the second heard event is next");
    check(!yonder_scripts_owed_fire(s,&out) && !yonder_scripts_owed_pending(s) && probe_text_is("out","hh"),
          "load events: the quiet rest are passed over in one drain, which ran no task");
    probe_drop();
}

static void load_event_cases(void) {
    load_loader_case();
    load_handler_only_case();
    load_before_scripts_case();
    load_owe_ring_case();
    load_sheets_case();
    load_scripts_case();
    load_modules_case();
    load_many_styles_case();
}
