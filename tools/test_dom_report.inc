/* P2a, A SCRIPT'S FAILURES (libdom/report.c, the ErrorEvent and
 * PromiseRejectionEvent sections of event.c, libjs's script host;
 * WINDOW_DOCUMENT.md § P2a): a case per sentence of report.c's contract,
 * the two interfaces, the switch off, a flood a page can make, and every
 * allocation a failure's events make failed in turn. A script runs as
 * yonder runs one, os64_js_run and then os64_dom_script_ended. */

#define REPORT_URL "https://example.test/dir/page.html"

/* What options.failed heard: each `task: message`, joined by `|`; and the
 * place of each one's stack (os64_dom_stack_frame), `line:col` or `-`,
 * joined the same way. */
static char report_failed[1024], report_failed_at[256];
static void report_failed_hook(void *opaque, const char *task, const char *message, const char *stack)
{
    check(opaque == NULL, "report: the failed hook's host context is the options'");
    check(stack != NULL, "report: the failed hook is always given a stack, \"\" for none");
    size_t at = strlen(report_failed);
    snprintf(report_failed + at, sizeof(report_failed) - at, "%s%s: %s", at ? "|" : "", task, message);
    os64_dom_frame_t frame;
    at = strlen(report_failed_at);
    if (stack != NULL && os64_dom_stack_frame(stack, &frame))
        snprintf(report_failed_at + at, sizeof(report_failed_at) - at, "%s%u:%u", at ? "|" : "", frame.line,
                 frame.column);
    else
        snprintf(report_failed_at + at, sizeof(report_failed_at) - at, "%s-", at ? "|" : "");
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
    char engine_name[32];
    check(os64_dom_script_name(f->binding, address, engine_name, sizeof(engine_name)),
          "report: the script is named by the binding");
    check(strncmp(engine_name, "os64-script:", 12) == 0, "report: the script's name is the binding's own");
    os64_js_run(f->runtime, source, strlen(source), engine_name, outcome);
    os64_js_outcome_t jobs = *outcome;
    while (outcome->status == OS64_JS_EXCEPTION && jobs.jobs_pending) {
        os64_js_status_t r = os64_js_drain_jobs(f->runtime, UINT64_MAX, &jobs);
        if (r != OS64_JS_OK && r != OS64_JS_MORE_JOBS && r != OS64_JS_EXCEPTION) {
            *outcome = jobs;
            break;
        }
    }
    snprintf(outcome->source_name, sizeof(outcome->source_name), "%s", name);
    check(os64_dom_script_ended(f->binding, name, outcome), "report: the script's end is settled");
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
    /* Writing a task's first report down runs none of the page's code: a
     * fresh binding, so the getters' error is the first it keeps, reported
     * by reportError and thrown by a nested dispatch's listener. */
    options.url = REPORT_URL;
    for (int nested = 0; nested < 2; nested++) {
        f = fixture_new(&options);
        if (!fixture_ready(&f)) return;
        dom_script(&f, STORAGE_ASSERT
            "globalThis.calls=0;const e=new Error('x');"
            "Object.defineProperty(e,'name',{get(){calls++;return 'Named'}});"
            "Object.defineProperty(e,'message',{get(){calls++;return 'said'}});"
            "e.toString=()=>{calls++;return 'converted'};",
            "report: an error whose reading would run the page's code");
        dom_script(&f, nested
            ? "const d=document.createElement('div');d.addEventListener('click',()=>{throw e});"
              "d.dispatchEvent(new Event('click'))"
            : "reportError(e)", "report: it is reported");
        check(os64_dom_take_report(f.binding, &outcome) && strstr(outcome.message, "converted") == NULL,
              "report: the first report is kept as the binding reads it");
        dom_script(&f, STORAGE_ASSERT "assert(calls===0,'calls '+calls)",
                   nested ? "report: a nested listener's first error is written down running no getter"
                          : "report: reportError's first value is written down running no getter");
        fixture_free(&f);
    }
}

/* HTML'S MUTED ERRORS (report.c): whatever another site's script throws,
 * rejects or reports is "Script error." or no event at all, decided by the
 * script that was running, which the binding knows by its own name for
 * each script and no address or stack text decides. Each case is a way
 * past an earlier design, and the page's own script is still told whole. */
#define MUTED_SETUP "globalThis.log=[];onerror=function(m,s,l,c,e){log.push(['error',m,s,l,e&&e.message||e].join(' '))};" \
    "onunhandledrejection=e=>log.push('unhandled '+(e.reason&&e.reason.message||e.reason));" \
    "onrejectionhandled=e=>log.push('handled '+(e.reason&&e.reason.message||e.reason));"
#define MUTED_ERROR "'error Script error.  0 '"

static void dom_report_muting_cases(void)
{
    os64_dom_options_t options = report_options(true);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    os64_js_outcome_t outcome;
    bool prevented;
    os64_dom_event_t resize = {.type = "resize"}, scroll = {.type = "scroll"};
    report_failed[0] = '\0';
    dom_script(&f, MUTED_SETUP, "muting: the page listens for everything");
    /* An address with "?(" in it once made the stack's place unreadable. */
    report_script(&f, "addEventListener('resize',function(){throw new Error('their secret A')})", "lib.js",
                  "https://other.test/lib.js?(", &outcome);
    check(os64_dom_dispatch(f.binding, NULL, &resize, &prevented, &outcome) == OS64_JS_EXCEPTION &&
          strstr(outcome.message, "their secret A") != NULL,
          "muting: another site's listener throws, the record's whole failure");
    report_log_is(&f, MUTED_ERROR, "muting: an address the stack text could not name is muted all the same");
    fixture_free(&f);

    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    report_failed[0] = '\0';
    dom_script(&f, MUTED_SETUP, "muting: the page listens again");
    /* A value that is no Error carries no place a stack could name. */
    report_script(&f, "addEventListener('resize',function(){Promise.reject('their secret B2');throw 'their secret B1'})",
                  "lib.js", "https://other.test/lib.js", &outcome);
    check(os64_dom_dispatch(f.binding, NULL, &resize, &prevented, &outcome) == OS64_JS_EXCEPTION &&
          strstr(outcome.message, "their secret B1") != NULL && strstr(report_failed, "their secret B2") != NULL,
          "muting: another site's listener throws and rejects values that are no Error, the record's whole");
    report_log_is(&f, MUTED_ERROR, "muting: the page hears \"Script error.\" and no rejection event");
    check(report_script(&f, "throw 'their secret E'", "top.js", "https://other.test/top.js", &outcome) ==
          OS64_JS_EXCEPTION, "muting: another site's script throws a string at its top level");
    report_log_is(&f, MUTED_ERROR, "muting: which is \"Script error.\" too");
    /* A muted rejection a handler reaches later, its own script's or the
     * page's, is no rejectionhandled either. */
    check(report_script(&f, "const p=Promise.reject(new Error('their secret C'));globalThis.theirs=Promise.reject('their secret C2');"
                        "addEventListener('scroll',function(){p.catch(()=>{})})",
                        "late.js", "https://other.test/late.js", &outcome) == OS64_JS_UNHANDLED_REJECTION &&
          strstr(outcome.message, "their secret C") != NULL, "muting: another site's script leaves two rejections");
    report_log_is(&f, "''", "muting: and no unhandledrejection is fired for them");
    check(os64_dom_dispatch(f.binding, NULL, &scroll, &prevented, &outcome) == OS64_JS_OK,
          "muting: its own listener handles one later");
    dom_script(&f, "theirs.catch(()=>{})", "muting: the page handles the other");
    os64_dom_dispatch(f.binding, NULL, &scroll, &prevented, &outcome);
    report_log_is(&f, "''", "muting: neither is a rejectionhandled");
    check(report_script(&f, "reportError(new Error('their secret D'));reportError('their secret D2');"
                        "addEventListener('resize',function(){reportError(new Error('their secret D3'))})",
                        "report.js", "https://other.test/report.js", &outcome) == OS64_JS_EXCEPTION &&
          strstr(outcome.message, "their secret D") != NULL,
          "muting: another site's script reports errors, the first its outcome, whole");
    os64_dom_dispatch(f.binding, NULL, &resize, &prevented, &outcome);
    report_log_is(&f, MUTED_ERROR "+'|'+" MUTED_ERROR "+'|'+" MUTED_ERROR "+'|'+" MUTED_ERROR,
                  "muting: each is \"Script error.\", reported from its script or from its listener");
    fixture_free(&f);

    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, MUTED_SETUP, "muting: the page listens a third time");
    /* A callback whose own name says nothing (bound, made by eval) is the
     * script's that handed it over; a function of theirs the page calls
     * rejects as theirs. */
    report_script(&f, "addEventListener('resize',function(){throw new Error('their secret G1')}.bind(null));"
                  "addEventListener('scroll',(0,eval)('(function(){throw new Error(\"their secret G2\")})'));"
                  "queueMicrotask(function(){throw new Error('their secret G3')}.bind(null));"
                  "setTimeout(function(){throw new Error('their secret G4')}.bind(null));"
                  "globalThis.theirReject=function(){Promise.reject(new Error('their secret G5'))};",
                  "bound.js", "https://other.test/bound.js", &outcome);
    report_log_is(&f, MUTED_ERROR, "muting: their bound microtask's throw is \"Script error.\"");
    os64_dom_dispatch(f.binding, NULL, &resize, &prevented, &outcome);
    os64_dom_dispatch(f.binding, NULL, &scroll, &prevented, &outcome);
    check(os64_dom_timer_fire(f.binding, UINT64_MAX - 1, &outcome) && outcome.status == OS64_JS_EXCEPTION,
          "muting: their bound timer throws");
    report_log_is(&f, MUTED_ERROR "+'|'+" MUTED_ERROR "+'|'+" MUTED_ERROR,
                  "muting: their bound listener, eval's function and bound timer are \"Script error.\"");
    check(report_script(&f, "theirReject()", "page.html#inline-1", NULL, &outcome) == OS64_JS_UNHANDLED_REJECTION,
          "muting: the page calls their function, which rejects");
    report_log_is(&f, "''", "muting: their rejection is no event, though the page's script called it");
    /* The page's own script, external or inline, is told whole. */
    report_script(&f, "addEventListener('resize',function(){throw new Error('ours F')});"
                  "globalThis.ours=Promise.reject(new Error('ours F2'))", "own.js", "https://example.test/own.js",
                  &outcome);
    report_log_is(&f, "'unhandled ours F2'", "muting: the page's own rejection is told");
    dom_script(&f, "ours.catch(()=>{})", "muting: and handled later");
    os64_dom_dispatch(f.binding, NULL, &resize, &prevented, &outcome);
    report_log_is(&f, MUTED_ERROR "+'|error Uncaught Error: ours F https://example.test/own.js 1 ours F"
                  "|handled ours F2'", "muting: its listener's error and its rejectionhandled are told whole");
    fixture_free(&f);

    /* A dispatch inside a script calls the listeners' scripts and comes back
     * to the one that dispatched: what that script throws next is its own,
     * at any depth (Codex #339 rd2). */
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, MUTED_SETUP "addEventListener('custom',function(){});"
        "addEventListener('outer',function(){dispatchEvent(new Event('inner'));throw new Error('page')});",
        "muting: the page's listeners");
    report_script(&f, "addEventListener('foreign',function(){});addEventListener('inner',function(){})",
                  "theirs.js", "https://other.test/theirs.js", &outcome);
    report_script(&f, "dispatchEvent(new Event('custom'));throw new Error('foreign secret')", "nested.js",
                  "https://other.test/nested.js", &outcome);
    report_log_is(&f, MUTED_ERROR, "muting: another site's script that dispatched to the page's listener is muted");
    report_script(&f, "dispatchEvent(new Event('foreign'));\nthrow new Error('ours after')", "page.html#inline-1", NULL,
                  &outcome);
    report_log_is(&f, "'error Uncaught Error: ours after " REPORT_URL " 2 ours after'",
                  "muting: the page's script that dispatched to another site's listener is told whole");
    report_script(&f, "dispatchEvent(new Event('outer'));throw new Error('foreign two')", "two.js",
                  "https://other.test/two.js", &outcome);
    report_log_is(&f, "'error Uncaught Error: page " REPORT_URL " 1 page|'+" MUTED_ERROR,
                  "muting: two levels down and back, the page's listener is told whole and their script is muted");
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
    dom_script(&f, "setTimeout(()=>{queueMicrotask(()=>log.push('job'));throw Error('callback')},0)",
               "report: a timer callback queues a job and throws");
    check(os64_dom_timer_fire(f.binding, UINT64_MAX - 1, &outcome) && outcome.status == OS64_JS_EXCEPTION,
          "report: the callback's throw is the timer's failure");
    report_log_is(&f, "'error Uncaught Error: callback|job'",
                  "report: its error event comes before the checkpoint after it, as a listener's does");
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
    /* HTML queues rejectionhandled as a task: a handler an unhandledrejection
     * listener adds to an earlier promise has its reaction run first. */
    fixture_free(&f);
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, "globalThis.log=[];"
        "onunhandledrejection=e=>{log.push('unhandled '+e.reason);"
        "if(e.reason===2)first.catch(()=>log.push('catch'))};"
        "onrejectionhandled=e=>log.push('handled '+e.reason);", "report: a listener that handles an earlier one");
    report_script(&f, "globalThis.first=Promise.reject(1)", "page.html#inline-10", NULL, &outcome);
    dom_script(&f, "setTimeout(()=>{Promise.reject(2)},0)", "report: a timer that rejects");
    check(os64_dom_timer_fire(f.binding, UINT64_MAX - 1, &outcome) && outcome.status == OS64_JS_UNHANDLED_REJECTION,
          "report: the timer's rejection is its failure");
    report_log_is(&f, "'unhandled 1|unhandled 2|catch|handled 1'",
                  "report: the catch reaction runs before the rejectionhandled it earned");
    /* Each rejectionhandled is a task of its own in HTML, so the microtasks
     * one listener queues run before the next is fired (Codex #339 rd2);
     * an unhandledrejection's the same. */
    dom_script(&f, "log.length=0;onunhandledrejection=e=>{log.push('unhandled '+e.reason);"
        "queueMicrotask(()=>log.push('micro u'+e.reason))};"
        "onrejectionhandled=e=>{log.push('handled '+e.reason);queueMicrotask(()=>log.push('micro h'+e.reason))};",
        "report: listeners that queue microtasks");
    report_script(&f, "globalThis.one=Promise.reject(1);globalThis.two=Promise.reject(2)", "page.html#inline-11",
                  NULL, &outcome);
    report_script(&f, "one.catch(()=>{});two.catch(()=>{})", "page.html#inline-12", NULL, &outcome);
    report_log_is(&f, "'unhandled 1|micro u1|unhandled 2|micro u2|handled 1|micro h1|handled 2|micro h2'",
                  "report: each event's microtasks run before the next event");
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

/* The binding's own report is its task's whatever the task's outcome: a
 * task that reportError'd and left a rejection has the rejection as its
 * outcome and the report told to the host, and the next task, which does
 * nothing, reports nothing (the cold read's finding 6). */
static void dom_report_close_cases(void)
{
    os64_dom_options_t options = report_options(true);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    os64_js_outcome_t outcome;
    bool prevented;
    report_failed[0] = '\0';
    dom_script(&f, "addEventListener('resize',function(){reportError(new Error('first task'));"
               "Promise.reject('also first task')},{once:true});addEventListener('scroll',function(){})",
               "close: a task that reports and rejects");
    os64_dom_event_t resize = {.type = "resize"}, scroll = {.type = "scroll"};
    check(os64_dom_dispatch(f.binding, NULL, &resize, &prevented, &outcome) == OS64_JS_UNHANDLED_REJECTION &&
          !strcmp(outcome.message, "unhandled rejection: also first task") &&
          !strcmp(report_failed, "resize event: Error: first task"),
          "close: the rejection is the outcome and the report is told beside it, under the task's name");
    check(os64_dom_dispatch(f.binding, NULL, &scroll, &prevented, &outcome) == OS64_JS_OK,
          "close: and the next task, which does nothing, reports nothing");
    report_failed[0] = '\0';
    dom_script(&f, "globalThis.d=document.createElement('div');d.addEventListener('x',()=>{throw Error('nested')})",
               "close: a nested listener that throws");
    check(report_script(&f, "d.dispatchEvent(new Event('x'));throw Error('own')", "page.html#inline-9", NULL,
                        &outcome) == OS64_JS_EXCEPTION && strstr(outcome.message, "own") != NULL &&
          !strcmp(report_failed, "page.html#inline-9: Error: nested") && !os64_dom_take_report(f.binding, &outcome),
          "close: a script's nested error is told beside its own and taken");
    report_failed[0] = '\0';
    fixture_free(&f);
}

/* The bound on what one task tells the host (report.c's D_REJECTIONS_TOLD):
 * two hundred thousand rejections in one script, none heard by a listener,
 * are taken with no script running, so the host's work for them must not
 * be per rejection. The hook is called 17 times: sixteen lines, then one
 * that counts the rest. */
static size_t bound_calls;
static char bound_last[160];
static void bound_hook(void *opaque, const char *task, const char *message, const char *stack)
{
    (void)opaque; (void)task; (void)stack;
    bound_calls++;
    snprintf(bound_last, sizeof(bound_last), "%s", message);
}
static void dom_report_bound_cases(void)
{
    os64_dom_options_t options = report_options(true);
    options.failed = bound_hook;
    DomFixture f = fixture_base(0, 0, 64u << 20);
    if (!f.runtime || !f.state) return;
    os64_js_outcome_t outcome;
    f.binding = os64_dom_create(f.runtime, f.document, f.state, &options, &outcome);
    if (!fixture_ready(&f)) return;
    bound_calls = 0;
    check(report_script(&f, "for(let i=0;i<200000;i++)Promise.reject(i)", "flood.js", NULL, &outcome) ==
          OS64_JS_UNHANDLED_REJECTION && !strcmp(outcome.message, "unhandled rejection: 0"),
          "bound: two hundred thousand rejections, the first the script's failure");
    check(bound_calls == 17 && !strcmp(bound_last, "and 199983 more unhandled rejections"),
          "bound: sixteen are told one by one and the rest in one counted line");
    bound_calls = 0;
    check(report_script(&f, "Promise.reject('a');Promise.reject('b')", "after.js", NULL, &outcome) ==
          OS64_JS_UNHANDLED_REJECTION && bound_calls == 1 && !strcmp(bound_last, "unhandled rejection: b"),
          "bound: the next task counts afresh");
    fixture_free(&f);
}

/* A sweep's one run: the setup, then the operation, as a script and its end
 * (`listener` false) or as a resize listener's task (true), with
 * allocation `cut` of its kind refused (0: none); how many it made. The
 * listener is compiled before the cut, as the window's sweeps compile
 * theirs: the engine's compiler under a refused allocation is libjs's to
 * prove, not this binding's. */
static size_t report_sweep_run(const char *operation, const char *address, bool listener, bool engine,
                               size_t cut)
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
    if (listener) report_script(&f, source, "sweep-listener.js", address, &outcome);
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
    } else report_script(&f, operation, "sweep.js", address, &outcome);
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
    /* The second is another site's script, so muting's own allocations
     * (the set its rejections are kept in) are refused in turn too. */
    static const char *const kOperations[] = {
        "Promise.reject(new Error('swept'));queueMicrotask(()=>{throw 3});later.catch(()=>{});"
        "reportError(new TypeError('r'));throw Error('own')",
        "Promise.reject(new Error('theirs'));Promise.reject(2);reportError(1);throw Error('theirs')",
    };
    static const char *const kAddresses[] = {NULL, "https://other.test/sweep.js"};
    for (size_t op = 0; op < sizeof(kOperations) / sizeof(kOperations[0]); op++)
        for (int listener = 0; listener < 2; listener++) {
            size_t count = report_sweep_run(kOperations[op], kAddresses[op], listener, listener, 0);
            printf("report operation %zu %s allocation points: %zu\n", op, listener ? "engine" : "binding", count);
            for (size_t cut = 1; cut <= count; cut++) {
                report_sweep_run(kOperations[op], kAddresses[op], listener, listener, cut);
                dom_sweeps++;
            }
        }
}

/* A stack's first placed frame (os64_dom_stack_frame), and the stacks the
 * failures the binding writes itself carry: a listener's error, an
 * unhandled rejection, and one told to the host beside the outcome. */
static bool frame_is(const char *stack, const char *function, const char *file, uint32_t line, uint32_t column)
{
    os64_dom_frame_t frame;
    if (!os64_dom_stack_frame(stack, &frame)) return file == NULL;
    bool named = function == NULL ? frame.function == NULL && frame.function_length == 0
        : frame.function != NULL && frame.function_length == strlen(function) &&
          !memcmp(frame.function, function, frame.function_length);
    return file != NULL && named && frame.file_length == strlen(file) && !memcmp(frame.file, file, frame.file_length) &&
           frame.line == line && frame.column == column;
}

static void dom_report_frame_cases(void)
{
    check(frame_is("    at f (os64-script:3:12:5)\n    at os64-script:3:20:1\n", "f", "os64-script:3", 12, 5),
          "frame: the first frame, its function, file, line and column");
    check(frame_is("    at Array.prototype.map (native)\n    at <anonymous> (os64-script:0:4:9)\n", NULL,
                   "os64-script:0", 4, 9),
          "frame: a native frame is passed over, and an anonymous function has no name");
    check(frame_is("    at <eval> (os64-script:0:1:7)\n", NULL, "os64-script:0", 1, 7),
          "frame: nor has a script's own code");
    check(frame_is("    at os64-script:2:7:3\n", NULL, "os64-script:2", 7, 3),
          "frame: a script that did not compile, its file alone");
    check(frame_is("    at get value (os64-script:1:2:3)\n", "get value", "os64-script:1", 2, 3),
          "frame: a name with a space in it");
    check(frame_is("    at <input>:1:1\n    at parse (native)\n    at f (os64-script:3:1:9)\n", "f", "os64-script:3", 1, 9) &&
          frame_is("    at <body>:1:1\n    at g (os64-script:4:2:5)\n", "g", "os64-script:4", 2, 5),
          "frame: JSON text names no place: the script that parsed it does");
    check(frame_is("    at f (os64-script:3:12", NULL, NULL, 0, 0) &&
          frame_is("    at f (os64-script:3:12:5)\n    at g (os64-scr", "f", "os64-script:3", 12, 5),
          "frame: a line the cap cut short is no frame");
    check(frame_is("    at f (os64-script:1)\n    at g (native)\n", NULL, NULL, 0, 0) &&
          frame_is("", NULL, NULL, 0, 0) && frame_is(NULL, NULL, NULL, 0, 0),
          "frame: none that names a line, an empty stack and none at all");

    os64_dom_options_t options = report_options(true);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    os64_js_outcome_t outcome;
    bool prevented;
    os64_dom_event_t resize = {.type = "resize"};
    dom_script(&f, "addEventListener('resize',function(){\n  throw new Error('listener')},{once:true})",
               "frame: a listener that throws");
    os64_dom_frame_t frame;
    check(os64_dom_dispatch(f.binding, NULL, &resize, &prevented, &outcome) == OS64_JS_EXCEPTION &&
          !strcmp(outcome.message, "Error: listener") && os64_dom_stack_frame(outcome.stack_trace, &frame) &&
          frame.line == 2,
          "frame: a listener's error carries its stack, whose first frame is the line that threw");
    report_failed[0] = report_failed_at[0] = '\0';
    check(report_script(&f, "Promise.reject(new Error('one'));\n\n  Promise.reject(new Error('two'));Promise.reject(3)",
                        "page.html#inline-2", NULL, &outcome) == OS64_JS_UNHANDLED_REJECTION &&
          os64_dom_stack_frame(outcome.stack_trace, &frame) && frame.line == 1,
          "frame: a rejection's outcome carries its reason's stack");
    check(!strcmp(report_failed, "page.html#inline-2: unhandled rejection: Error: two|"
                                 "page.html#inline-2: unhandled rejection: 3") &&
          !strncmp(report_failed_at, "3:", 2) && strstr(report_failed_at, "|-") != NULL,
          "frame: and one told to the host its own, a primitive none");
    dom_script(&f, "globalThis.d=document.createElement('div');d.addEventListener('x',()=>{\n\n  throw Error('nested')})",
               "frame: a nested listener that throws");
    report_failed[0] = report_failed_at[0] = '\0';
    check(report_script(&f, "d.dispatchEvent(new Event('x'));throw Error('own')", "page.html#inline-3", NULL,
                        &outcome) == OS64_JS_EXCEPTION &&
          !strcmp(report_failed, "page.html#inline-3: Error: nested") && !strncmp(report_failed_at, "3:", 2),
          "frame: an error the binding reported itself carries its stack, told beside the script's own");
    bool page = false;
    check(os64_dom_stack_frame(outcome.stack_trace, &frame) && os64_dom_frame_script(f.binding, &frame, &page) == NULL &&
          page, "frame: an inline script's frame names the page");
    check(report_script(&f, "throw new Error('lib')", "lib.js", "https://example.test/js/lib.js", &outcome) ==
          OS64_JS_EXCEPTION && os64_dom_stack_frame(outcome.stack_trace, &frame) &&
          os64_dom_frame_script(f.binding, &frame, &page) != NULL &&
          !strcmp(os64_dom_frame_script(f.binding, &frame, &page), "https://example.test/js/lib.js") && !page,
          "frame: a src script's frame names its address");
    check(os64_dom_stack_frame("    at onclick (onclick attribute:1:2)\n", &frame) &&
          os64_dom_frame_script(f.binding, &frame, &page) == NULL && !page,
          "frame: and a file that is none of the binding's names, none");
    report_failed[0] = report_failed_at[0] = '\0';
    fixture_free(&f);
}

static void dom_report_cases(void)
{
    dom_report_frame_cases();
    dom_report_error_cases();
    dom_report_muting_cases();
    dom_report_nested_cases();
    dom_report_task_cases();
    dom_report_rejection_cases();
    dom_report_interface_cases();
    dom_report_switch_cases();
    dom_report_flood_cases();
    dom_report_bound_cases();
    dom_report_close_cases();
}
