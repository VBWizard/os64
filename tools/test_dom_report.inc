/* P2a, A SCRIPT'S FAILURES (libdom/report.c, the ErrorEvent and
 * PromiseRejectionEvent sections of event.c, libjs's script host;
 * WINDOW_DOCUMENT.md § P2a): a case per sentence of report.c's contract,
 * the two interfaces, the switch off, a flood a page can make, and every
 * allocation a failure's events make failed in turn. A script runs as
 * yonder runs one, os64_js_run and then os64_dom_script_ended. */

#define REPORT_URL "https://example.test/dir/page.html"

/* What options.failed heard: each `task: message`, joined by `|`. */
static char report_failed[1024];
static void report_failed_hook(void *opaque, const char *task, const char *message)
{
    check(opaque == NULL, "report: the failed hook's host context is the options'");
    size_t at = strlen(report_failed);
    snprintf(report_failed + at, sizeof(report_failed) - at, "%s%s: %s", at ? "|" : "", task, message);
}

static os64_dom_options_t report_options(bool modern)
{
    os64_dom_options_t options = os64_dom_default_options();
    options.url = REPORT_URL;
    options.modern = modern;
    options.failed = report_failed_hook;
    return options;
}

/* A script as yonder runs one (scripts.c's run_item): its outcome, the
 * script's own failure first, the jobs a script that threw left drained,
 * and then what it left owed. */
static os64_js_status_t report_script(DomFixture *f, const char *source, const char *name, const char *address,
                                      os64_js_outcome_t *outcome)
{
    os64_js_run(f->runtime, source, strlen(source), name, outcome);
    os64_js_outcome_t jobs = *outcome;
    while (outcome->status == OS64_JS_EXCEPTION && jobs.jobs_pending) {
        os64_js_status_t r = os64_js_drain_jobs(f->runtime, UINT64_MAX, &jobs);
        if (r != OS64_JS_OK && r != OS64_JS_MORE_JOBS && r != OS64_JS_EXCEPTION) {
            *outcome = jobs;
            break;
        }
    }
    check(os64_dom_script_ended(f->binding, name, address, outcome), "report: the script's end is settled");
    return outcome->status;
}

/* The page's `log`, joined, compared, and emptied. */
static bool report_log_is(DomFixture *f, const char *expected, const char *what)
{
    char source[1024];
    snprintf(source, sizeof(source),
             "if(log.join('|')!==%s)throw Error('log: '+log.join('|'));log.length=0;", expected);
    return dom_script(f, source, what);
}

static void dom_report_error_cases(void)
{
    os64_dom_options_t options = report_options(true);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    os64_js_outcome_t outcome;
    dom_script(&f, STORAGE_ASSERT
        "globalThis.log=[];"
        "assert(onerror===null&&document.onerror===null&&onunhandledrejection===null&&onrejectionhandled===null);"
        "assert(!('onerror' in document.body)&&!('onerror' in document.createElement('img')),"
        "'an element has no onerror: nothing fires error at one');"
        "onerror=function(message,source,lineno,colno,error){"
        "log.push([typeof message,message,source,lineno,colno,error&&error.message].join(' '));};",
        "report: window.onerror set");
    check(report_script(&f, "\n  missing();", "page.html#inline-1", NULL, &outcome) == OS64_JS_EXCEPTION &&
          strstr(outcome.message, "missing") != NULL && !strcmp(outcome.source_name, "page.html#inline-1"),
          "report: a script's uncaught exception is still its outcome");
    report_log_is(&f, "\"string Uncaught ReferenceError: 'missing' is not defined " REPORT_URL
                  " 2 3 'missing' is not defined\"",
                  "report: and window.onerror heard it, with its message, the page, its line and column");
    check(report_script(&f, "throw new TypeError('from a file')", "lib.js", "https://example.test/js/lib.js",
                        &outcome) == OS64_JS_EXCEPTION, "report: an external script throws");
    report_log_is(&f, "'string Uncaught TypeError: from a file https://example.test/js/lib.js 1 20 from a file'",
                  "report: its error event names the script's own address");
    dom_script(&f, "onunhandledrejection=e=>log.push('rejection '+e.reason)", "report: rejections heard");
    check(report_script(&f, "Promise.reject('secret');throw new TypeError('their secret')", "other.js",
                        "https://other.test/other.js", &outcome) == OS64_JS_EXCEPTION &&
          strstr(outcome.message, "their secret") != NULL,
          "report: a script from another site throws, which is its failure as any script's");
    report_log_is(&f, "'string Script error.  0 0 '",
                  "report: and the page is told \"Script error.\" and nothing more, and none of its rejections");
    check(report_script(&f, "Promise.reject('ours')", "data.js", "data:text/javascript,x", &outcome) ==
          OS64_JS_UNHANDLED_REJECTION, "report: a data: script's rejection");
    report_log_is(&f, "'rejection ours'", "report: is no other site's");
    /* Code another site's script defined, failing later in the page's own
     * task: told as that site's, by the place the engine says it failed. */
    check(report_script(&f, "addEventListener('resize',function(){theirSecretName()});"
                        "addEventListener('resize',async function(){theirOtherName()})",
                        "https://other.test/lib.js", "https://example.test/same.js", &outcome) == OS64_JS_OK,
          "report: a script named for another site defines two listeners");
    os64_dom_event_t resize = {.type = "resize"};
    bool prevented;
    check(os64_dom_dispatch(f.binding, NULL, &resize, &prevented, &outcome) == OS64_JS_EXCEPTION &&
          strstr(outcome.message, "theirSecretName") != NULL,
          "report: they throw and reject, the dispatch's failure as any is");
    report_log_is(&f, "'string Script error.  0 0 '",
                  "report: the page hears \"Script error.\", and no rejection event for the other");
    dom_script(&f, "onunhandledrejection=null", "report: rejections no longer heard");
    dom_script(&f,
        "onerror=function(){log.push('returns true');return true};"
        "addEventListener('error',e=>log.push(['listener',e.defaultPrevented,e instanceof ErrorEvent,"
        "e.cancelable,e.isTrusted,e.error].join(' ')));", "report: a handler that cancels, and a listener");
    check(report_script(&f, "throw 5", "page.html#inline-2", NULL, &outcome) == OS64_JS_EXCEPTION,
          "report: a cancelled error is the script's failure all the same");
    report_log_is(&f, "'returns true|listener true true true true 5'",
                  "report: window's onerror returning true cancels, as no other handler's true does");
    dom_script(&f, "onerror=function(){log.push('handler');return false};"
        "document.onerror=function(){log.push('document')};", "report: false does not cancel an error event");
    check(report_script(&f, "throw null", "page.html#inline-3", NULL, &outcome) == OS64_JS_EXCEPTION,
          "report: throw null");
    report_log_is(&f, "'handler|listener false true true true '",
                  "report: false is no cancel here, and the document's onerror is not on the path");
    dom_script(&f, "onerror=null;removeEventListener;globalThis.ran=0;"
        "onerror=function(){ran++;undefined_in_onerror};", "report: a handler that throws");
    check(report_script(&f, "throw Error('first')", "page.html#inline-4", NULL, &outcome) == OS64_JS_EXCEPTION &&
          strstr(outcome.message, "first") != NULL, "report: the script's own error is the outcome");
    dom_script(&f, STORAGE_ASSERT "assert(ran===1,'ran '+ran);log.length=0;",
               "report: an error inside the error handler is reported to no page");
    fixture_free(&f);
    /* An origin is the address's scheme, host and port: a query the address
     * parser would refuse (a space a host left unescaped) changes nothing. */
    options.url = "https://example.test/page.html?a variant";
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, "globalThis.log=[];onerror=function(message){log.push(message)}", "report: a page with a query");
    check(report_script(&f, "throw new Error('ours')", "https://example.test/lib.js", "https://example.test/lib.js",
                        &outcome) == OS64_JS_EXCEPTION, "report: its own script throws");
    report_log_is(&f, "'Uncaught Error: ours'", "report: and is not taken for another site's");
    fixture_free(&f);
}

static void dom_report_nested_cases(void)
{
    os64_dom_options_t options = report_options(true);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    os64_js_outcome_t outcome;
    dom_script(&f, STORAGE_ASSERT
        "globalThis.log=[];addEventListener('error',e=>log.push('error '+e.message+' '+(e.error===null)));"
        "const div=document.createElement('div');div.addEventListener('click',()=>{throw null});"
        "assert(div.dispatchEvent(new Event('click'))===true);"
        "assert(log.join()==='error Uncaught null true','the error event is fired before dispatchEvent returns: '+log.join());"
        "log.length=0;",
        "report: a nested listener's error event is synchronous");
    check(os64_dom_take_report(f.binding, &outcome) && !strcmp(outcome.message, "null"),
          "report: and its error is the script's report as before");
    dom_script(&f, STORAGE_ASSERT
        "reportError(new TypeError('reported'));reportError(1);"
        "throws('TypeError',()=>reportError());assert(reportError.length===1&&reportError.name==='reportError');"
        "assert(log.join('|')==='error Uncaught TypeError: reported false|error Uncaught 1 false',log.join('|'));"
        "log.length=0;let read=false;"
        "reportError({get name(){read=true},get message(){read=true},get fileName(){read=true},"
        "get lineNumber(){read=true}});assert(!read,'reportError reads no getter');"
        "assert(log.join()==='error Uncaught an object that is not an Error false',log.join());log.length=0;"
        "function Assertion(m){this.message=m}Assertion.prototype=Object.create(Error.prototype);"
        "let trapped=false;const trap={get(){trapped=true},getOwnPropertyDescriptor(){trapped=true},"
        "getPrototypeOf(){trapped=true;return null}};"
        "reportError(new Assertion('like an error'));reportError({message:'just a message'});"
        "reportError(new Proxy(new Error('hidden'),trap));reportError(Object.setPrototypeOf(new Error(),new Proxy({},trap)));"
        "assert(!trapped,'no proxy trap runs');"
        "assert(log.join('|')==='error Uncaught Error: like an error false|error Uncaught just a message false|"
        "error Uncaught an object that is not an Error false|error Uncaught Error false',log.join('|'));log.length=0;",
        "report: reportError fires the error event now");
    check(os64_dom_take_report(f.binding, &outcome) && strstr(outcome.message, "reported") != NULL,
          "report: and the first error it reported is the script's");
    dom_script(&f, STORAGE_ASSERT
        "let place;addEventListener('error',function f(e){place=[e.filename,e.lineno>0,e.colno>0];"
        "removeEventListener('error',f)});\nreportError(1);"
        "assert(place.join()==='" REPORT_URL ",true,true',place.join());log.length=0;",
        "report: a value with no stack is placed where reportError was called");
    os64_dom_take_report(f.binding, &outcome);
    /* A body's onerror is window's, a function of five. */
    dom_script(&f, STORAGE_ASSERT
        "document.body.setAttribute('onerror','log.push([typeof event,source,lineno,colno,typeof error].join(\" \"))');"
        "assert(typeof onerror==='function'&&onerror.length===5&&document.body.onerror===undefined);",
        "report: the body's onerror attribute");
    check(report_script(&f, "throw Error('to the body')", "page.html#inline-5", NULL, &outcome) == OS64_JS_EXCEPTION,
          "report: a script throws");
    report_log_is(&f, "'error Uncaught Error: to the body false|string " REPORT_URL " 1 12 object'",
                  "report: the body's handler is called with the five");
    fixture_free(&f);
}

/* A host task's failures: a listener's, a timer's, a microtask's. */
static void dom_report_task_cases(void)
{
    os64_dom_options_t options = report_options(true);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    os64_js_outcome_t outcome;
    bool prevented;
    dom_script(&f,
        "globalThis.log=[];addEventListener('error',e=>log.push('error '+e.message));"
        "addEventListener('resize',()=>{Promise.resolve().then(()=>log.push('job'));throw Error('listener')});"
        "addEventListener('resize',()=>log.push('next'));",
        "report: listeners that throw");
    os64_dom_event_t resize = {.type = "resize"};
    check(os64_dom_dispatch(f.binding, NULL, &resize, &prevented, &outcome) == OS64_JS_EXCEPTION &&
          strstr(outcome.message, "listener") != NULL, "report: a listener's throw is its task's failure");
    report_log_is(&f, "'error Uncaught Error: listener|job|next'",
                  "report: its error event comes before the checkpoint after it");
    dom_script(&f,
        "setTimeout(()=>{queueMicrotask(()=>{throw new RangeError('micro')});log.push('timer')},0);"
        "setTimeout('throw new SyntaxError(\"as a string\")',0);",
        "report: a timer queues a throwing microtask");
    check(os64_dom_timer_fire(f.binding, UINT64_MAX - 1, &outcome) && outcome.status == OS64_JS_EXCEPTION &&
          strstr(outcome.message, "micro") != NULL, "report: the microtask's throw is the timer's failure");
    report_log_is(&f, "'timer|error Uncaught RangeError: micro'", "report: and the window heard it");
    check(os64_dom_timer_fire(f.binding, UINT64_MAX - 1, &outcome) && outcome.status == OS64_JS_EXCEPTION,
          "report: a string timer throws");
    report_log_is(&f, "'error Uncaught SyntaxError: as a string'", "report: and the window heard that too");
    fixture_free(&f);
}

static void dom_report_rejection_cases(void)
{
    os64_dom_options_t options = report_options(true);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    os64_js_outcome_t outcome;
    bool prevented;
    report_failed[0] = '\0';
    dom_script(&f, "globalThis.log=[];"
        "onunhandledrejection=e=>log.push(['unhandled',e.reason&&e.reason.message,e instanceof PromiseRejectionEvent,"
        "e.cancelable,e.isTrusted,e.promise instanceof Promise].join(' '));"
        "onrejectionhandled=e=>log.push('handled '+(e.reason&&e.reason.message)+' '+e.cancelable);",
        "report: rejection handlers");
    check(report_script(&f, "globalThis.kept=Promise.reject(new Error('lost'))", "page.html#inline-1", NULL,
                        &outcome) == OS64_JS_UNHANDLED_REJECTION &&
          !strcmp(outcome.message, "unhandled rejection: Error: lost") &&
          !strcmp(outcome.source_name, "page.html#inline-1"),
          "report: an unhandled rejection is the script's failure, said as one");
    report_log_is(&f, "'unhandled lost true true true true'", "report: and unhandledrejection was fired for it");
    check(report_script(&f, "Promise.reject(1).catch(()=>{});(async()=>{try{await Promise.reject(2)}catch(e){}})();"
                        "Promise.reject(3).then(null,()=>{})", "page.html#inline-2", NULL, &outcome) == OS64_JS_OK,
          "report: a caught rejection is no failure");
    report_log_is(&f, "''", "report: and fires nothing");
    check(report_script(&f, "const p=Promise.reject(4);Promise.resolve().then(()=>p.catch(()=>{}))",
                        "page.html#inline-3", NULL, &outcome) == OS64_JS_OK,
          "report: a handler added by a microtask is in time");
    check(report_script(&f, "kept.catch(()=>{})", "page.html#inline-4", NULL, &outcome) == OS64_JS_OK,
          "report: a handler added to a reported promise in a later task");
    report_log_is(&f, "'handled lost false'", "report: is its rejectionhandled");
    /* Chris's case: an async function that throws in a click handler. */
    dom_script(&f, "addEventListener('resize',async()=>{throw new TypeError('in a click')})",
               "report: an async listener");
    os64_dom_event_t resize = {.type = "resize"};
    check(os64_dom_dispatch(f.binding, NULL, &resize, &prevented, &outcome) == OS64_JS_UNHANDLED_REJECTION &&
          !strcmp(outcome.message, "unhandled rejection: TypeError: in a click") &&
          !strcmp(outcome.source_name, "resize event"),
          "report: an async listener's throw is its dispatch's unhandled rejection");
    report_log_is(&f, "'unhandled in a click true true true true'", "report: and an unhandledrejection");
    dom_script(&f, "onunhandledrejection=e=>{log.push('cancelled');e.preventDefault()};"
        "onrejectionhandled=null;",
        "report: a listener that cancels");
    check(report_script(&f, "Promise.reject(new Error('cancelled'))", "page.html#inline-5", NULL, &outcome) ==
          OS64_JS_OK, "report: a rejection a listener cancelled is no failure");
    report_log_is(&f, "'cancelled'", "report: its event ran");
    dom_script(&f, "onunhandledrejection=e=>{log.push('attached');e.promise.catch(()=>{})};"
        "onrejectionhandled=()=>log.push('rejectionhandled');", "report: a listener that handles it");
    check(report_script(&f, "Promise.reject(new Error('caught late'))", "page.html#inline-6", NULL, &outcome) ==
          OS64_JS_UNHANDLED_REJECTION, "report: handled inside its event is still unhandled when reported");
    dom_script(&f, "Promise.resolve()", "report: a later task");
    check(report_script(&f, "0", "page.html#inline-7", NULL, &outcome) == OS64_JS_OK,
          "report: nothing more is owed");
    report_log_is(&f, "'attached'", "report: and a handler added during the event is no rejectionhandled");
    dom_script(&f, "onunhandledrejection=null;onrejectionhandled=null;"
        "addEventListener('unhandledrejection',e=>{log.push(String(e.reason));"
        "if(e.reason<3)Promise.reject(e.reason+1)});", "report: a listener that rejects again");
    check(report_script(&f, "Promise.reject(0)", "page.html#inline-8", NULL, &outcome) ==
          OS64_JS_UNHANDLED_REJECTION && !strcmp(outcome.message, "unhandled rejection: 0"),
          "report: the first unhandled rejection is the task's failure");
    report_log_is(&f, "'0|1|2|3'", "report: and the ones its listeners made are fired in the same task");
    check(!strcmp(report_failed, "page.html#inline-8: unhandled rejection: 1|page.html#inline-8: unhandled rejection: 2|"
                  "page.html#inline-8: unhandled rejection: 3"),
          "report: and each after the first is told to the host as a failure of the same task");
    report_failed[0] = '\0';
    check(report_script(&f, "Promise.reject('a');throw Error('thrown')", "page.html#inline-9", NULL, &outcome) ==
          OS64_JS_EXCEPTION && strstr(outcome.message, "thrown") != NULL &&
          !strcmp(report_failed, "page.html#inline-9: unhandled rejection: a"),
          "report: a rejection a script that threw left is told to the host beside the script's exception");
    report_failed[0] = '\0';
    dom_script(&f, "addEventListener('resize',()=>{Promise.reject('x');Promise.reject('y')})",
               "report: a second listener leaves two more");
    check(os64_dom_dispatch(f.binding, NULL, &resize, &prevented, &outcome) == OS64_JS_UNHANDLED_REJECTION &&
          !strcmp(outcome.message, "unhandled rejection: TypeError: in a click") &&
          !strcmp(report_failed, "resize event: unhandled rejection: x|resize event: unhandled rejection: y"),
          "report: a dispatch's first rejection is its outcome and the rest the host's");
    report_log_is(&f, "'a|TypeError: in a click|x|y'", "report: each was fired");
    report_failed[0] = '\0';
    fixture_free(&f);
}

/* The interfaces a page can make. */
static void dom_report_interface_cases(void)
{
    os64_dom_options_t options = report_options(true);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, STORAGE_ASSERT
        "let e=new ErrorEvent('error');"
        "assert(e.message===''&&e.filename===''&&e.lineno===0&&e.colno===0&&e.error===undefined&&!e.cancelable);"
        "const err=new Error('x');let order=[];"
        "e=new ErrorEvent('custom',{get message(){order.push('message');return 'm'},"
        "get filename(){order.push('filename');return 'f\\uD800'},get lineno(){order.push('lineno');return -1},"
        "get colno(){order.push('colno');return 4.9},get error(){order.push('error');return err},"
        "get bubbles(){order.push('bubbles');return true},get cancelable(){order.push('cancelable');return true}});"
        "assert(order.join()==='bubbles,cancelable,colno,error,filename,lineno,message',order.join());"
        "assert(e.type==='custom'&&e.message==='m'&&e.filename==='f\\uFFFD'&&e.lineno===4294967295&&e.colno===4);"
        "assert(e.error===err&&e.bubbles&&e.cancelable&&!e.isTrusted&&e instanceof Event);"
        "assert(Object.getPrototypeOf(ErrorEvent)===Event&&ErrorEvent.length===1);"
        "throws('TypeError',()=>ErrorEvent('error'));throws('TypeError',()=>new ErrorEvent());"
        "throws('TypeError',()=>Object.getOwnPropertyDescriptor(ErrorEvent.prototype,'message').get.call(new Event('x')));"
        "class Mine extends ErrorEvent{};assert(new Mine('x') instanceof Mine);"
        "const p=new Promise(()=>{});"
        "let r=new PromiseRejectionEvent('x',{promise:p});"
        "assert(r.promise===p&&r.reason===undefined&&!r.bubbles&&!r.cancelable&&PromiseRejectionEvent.length===2);"
        "r=new PromiseRejectionEvent('x',{promise:p,reason:null,bubbles:true,cancelable:true});"
        "assert(r.reason===null&&r.bubbles&&r.cancelable);"
        "assert(new PromiseRejectionEvent('x',{promise:{}}).promise instanceof Object,'any object is a promise here');"
        "throws('TypeError',()=>new PromiseRejectionEvent('x',{bubbles:false}));"
        "throws('TypeError',()=>new PromiseRejectionEvent('x',{promise:5}));"
        "throws('TypeError',()=>new PromiseRejectionEvent('x'));"
        "throws('TypeError',()=>PromiseRejectionEvent('x',{promise:p}));"
        "throws('TypeError',()=>Object.getOwnPropertyDescriptor(PromiseRejectionEvent.prototype,'reason').get.call(e));"
        /* A constructed one dispatched at window is an ordinary event: no
         * five arguments for a handler, and no record. */
        "let heard;onerror=function(){heard=arguments.length};"
        "dispatchEvent(new Event('error'));assert(heard===1,'a plain error event: one argument');"
        "dispatchEvent(new ErrorEvent('error',{message:'made'}));assert(heard===5,'an ErrorEvent: five');",
        "report: ErrorEvent and PromiseRejectionEvent");
    fixture_free(&f);
}

/* With the switch off: no member, no event, and the record still says. */
static void dom_report_switch_cases(void)
{
    os64_dom_options_t options = report_options(false);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    os64_js_outcome_t outcome;
    dom_script(&f, STORAGE_ASSERT
        "for(const n of ['onerror','onunhandledrejection','onrejectionhandled','reportError','ErrorEvent',"
        "'PromiseRejectionEvent'])assert(!(n in window),n);assert(!('onerror' in document));"
        "globalThis.log=[];addEventListener('error',()=>log.push('error'));"
        "addEventListener('unhandledrejection',()=>log.push('unhandled'));"
        "document.body.setAttribute('onerror','log.push(\"body\")');",
        "report: the switch off has no members");
    check(report_script(&f, "Promise.reject(new Error('off'))", "page.html#inline-1", NULL, &outcome) ==
          OS64_JS_UNHANDLED_REJECTION && !strcmp(outcome.message, "unhandled rejection: Error: off"),
          "report: with the switch off an unhandled rejection is still the script's failure");
    check(report_script(&f, "throw Error('off too')", "page.html#inline-2", NULL, &outcome) == OS64_JS_EXCEPTION,
          "report: and an exception its own");
    report_log_is(&f, "''", "report: and no event was fired for either");
    fixture_free(&f);
}

/* What a page can make the binding do: a thousand unhandled rejections in
 * one script and as many throwing microtasks as the fixture's job cap
 * allows, each an event (linear: the wrapper's table and the binding's two
 * lists), and a listener that rejects for ever, which ends at the task's
 * deadline as any loop does. */
static void dom_report_flood_cases(void)
{
    os64_dom_options_t options = report_options(true);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    os64_js_outcome_t outcome;
    dom_script(&f, "globalThis.n=0;globalThis.m=0;addEventListener('unhandledrejection',()=>n++);"
        "addEventListener('error',()=>m++)", "report: counters");
    check(report_script(&f, "for(let i=0;i<1000;i++)Promise.reject(i);", "flood.js", NULL, &outcome) ==
          OS64_JS_UNHANDLED_REJECTION && !strcmp(outcome.message, "unhandled rejection: 0"),
          "report: a thousand rejections, the first the failure");
    dom_script(&f, STORAGE_ASSERT "assert(n===1000,'n '+n)", "report: and a thousand events");
    os64_dom_event_t resize = {.type = "resize"};
    bool prevented;
    dom_script(&f, "addEventListener('resize',()=>{for(let i=0;i<90;i++)queueMicrotask(()=>{throw i})})",
               "report: a listener queues ninety throws");
    size_t held = os64_dom_registry_count(f.binding);
    check(os64_dom_dispatch(f.binding, NULL, &resize, &prevented, &outcome) == OS64_JS_EXCEPTION,
          "report: the first is the task's failure");
    check(os64_dom_registry_count(f.binding) == held, "report: nothing is left waiting");
    dom_script(&f, STORAGE_ASSERT "assert(m===90,'m '+m)", "report: and each was an error event");
    fixture_free(&f);
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, "addEventListener('unhandledrejection',()=>Promise.reject(0))", "report: a listener for ever");
    dom_clock_step = 1000;
    check(report_script(&f, "Promise.reject(0)", "forever.js", NULL, &outcome) == OS64_JS_LIMIT &&
          outcome.limit == OS64_JS_LIMIT_EXECUTION, "report: a rejection that makes another for ever is ended");
    dom_clock_step = 0;
    fixture_free(&f);
}

/* A sweep's one run: the setup, then the operation, as a script and its end
 * (`listener` false) or as a resize listener's task (true), with
 * allocation `cut` of its kind refused (0: none); how many it made. The
 * listener is compiled before the cut, as the window's sweeps compile
 * theirs: the engine's compiler under a refused allocation is libjs's to
 * prove, not this binding's. */
static size_t report_sweep_run(const char *operation, bool listener, bool engine, size_t cut)
{
    os64_dom_options_t options = report_options(true);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return 0;
    dom_script(&f, "onerror=()=>{};addEventListener('unhandledrejection',e=>e.reason);onrejectionhandled=()=>{};"
               "globalThis.later=Promise.reject(1);", "sweep: report setup");
    os64_js_outcome_t outcome;
    report_script(&f, "0", "sweep-setup.js", NULL, &outcome);
    char source[512];
    snprintf(source, sizeof(source), "addEventListener('resize',function(){%s})", operation);
    if (listener) dom_script(&f, source, "sweep: the operation as a listener");
    size_t *counter = engine ? &attempts : &dom_attempts;
    size_t start = *counter;
    if (cut != 0) {
        if (engine) fail_at = attempts + cut;
        else dom_fail_at = dom_attempts + cut;
    }
    if (listener) {
        os64_dom_event_t resize = {.type = "resize"};
        bool prevented;
        os64_dom_dispatch(f.binding, NULL, &resize, &prevented, &outcome);
    } else report_script(&f, operation, "sweep.js", NULL, &outcome);
    fail_at = 0;
    dom_fail_at = 0;
    size_t count = *counter - start;
    fixture_free(&f);
    return count;
}

/* Every binding allocation of a script that fails and of the events its
 * end fires, then every engine allocation of a listener's task that does
 * the same, refused in turn: nothing crashes, nothing leaks, and the
 * teardown balances (fixture_free). */
static void dom_report_sweep_cases(void)
{
    static const char *const kOperations[] = {
        "Promise.reject(new Error('swept'));queueMicrotask(()=>{throw 3});later.catch(()=>{});"
        "reportError(new TypeError('r'));throw Error('own')",
    };
    for (size_t op = 0; op < sizeof(kOperations) / sizeof(kOperations[0]); op++)
        for (int listener = 0; listener < 2; listener++) {
            size_t count = report_sweep_run(kOperations[op], listener, listener, 0);
            printf("report operation %zu %s allocation points: %zu\n", op, listener ? "engine" : "binding", count);
            for (size_t cut = 1; cut <= count; cut++) {
                report_sweep_run(kOperations[op], listener, listener, cut);
                dom_sweeps++;
            }
        }
}

static void dom_report_cases(void)
{
    dom_report_error_cases();
    dom_report_nested_cases();
    dom_report_task_cases();
    dom_report_rejection_cases();
    dom_report_interface_cases();
    dom_report_switch_cases();
    dom_report_flood_cases();
}
