/* Frame snapshots, cancellation at checkpoints, shared timestamps and bounded
 * execution: assertions use visible callback behavior, not queue internals. */
static void animation_spend(void *opaque,const char *text,size_t length)
{
    (void)opaque;(void)text;(void)length;
    dom_clock+=600000;
}

static void dom_animation_cases(void)
{
    p2b_ms=1000;p2b_us=1000000;
    os64_dom_options_t options=p2b_options(P2B_URL);
    DomFixture f=fixture_new(&options);
    if(!fixture_ready(&f))return;
    os64_js_outcome_t out;
    check(os64_dom_animation_next(f.binding)==UINT64_MAX &&
          !os64_dom_animation_fire(f.binding,p2b_ms,&out),"frame: empty queue sleeps");
    p2b_script(&f,
        "assert(requestAnimationFrame.length===1&&cancelAnimationFrame.length===1);"
        "throws('TypeError',()=>requestAnimationFrame());throws('TypeError',()=>requestAnimationFrame(2));"
        "throws('TypeError',()=>cancelAnimationFrame());throws('TypeError',()=>cancelAnimationFrame(Symbol()));"
        "globalThis.frames=[];globalThis.times=[];globalThis.removed=0;"
        "globalThis.first=requestAnimationFrame(function(t){frames.push('a');times.push(t);"
        "assert(this===window&&arguments.length===1);assert(t<=performance.now()&&t>=20);"
        "Promise.resolve().then(()=>{frames.push('micro');cancelAnimationFrame(removed);"
        "requestAnimationFrame(t=>{frames.push('next');times.push(t)})})});"
        "removed=requestAnimationFrame(()=>frames.push('cancelled'));"
        "globalThis.last=requestAnimationFrame(t=>{frames.push('b');times.push(t)});"
        "assert(first<removed&&removed<last);assert(cancelAnimationFrame(99999)===undefined);",
        "frame: request, signatures and conversion");
    check(os64_dom_animation_next(f.binding)==1020,"frame: nominal 20ms cadence");
    p2b_advance(19);
    check(!os64_dom_animation_fire(f.binding,p2b_ms,&out),"frame: not early");
    p2b_advance(1);
    check(os64_dom_animation_fire(f.binding,p2b_ms,&out)&&out.status==OS64_JS_OK,
          "frame: one batch with checkpoints");
    p2b_script(&f,"assert(frames.join()==='a,micro,b',frames.join());assert(times[0]===times[1]);",
               "frame: microtask cancellation, request order, same timestamp");
    check(!os64_dom_animation_fire(f.binding,p2b_ms,&out),"frame: request during callback deferred");
    p2b_advance(3000);
    check(os64_dom_animation_fire(f.binding,p2b_ms,&out)&&out.status==OS64_JS_OK,
          "frame: late wake runs one snapshot");
    p2b_script(&f,"assert(frames.join()==='a,micro,b,next');assert(times[2]>times[1]);",
               "frame: no accumulated frames after a delay");
    check(os64_dom_animation_next(f.binding)==UINT64_MAX,"frame: one-shot callbacks released");
    p2b_script(&f,
        "frames=[];addEventListener('error',e=>{frames.push('error');e.preventDefault()});"
        "requestAnimationFrame(()=>{throw Error('frame error')});"
        "requestAnimationFrame(()=>frames.push('after'));",
        "frame: throwing callback setup");
    p2b_advance(20);
    check(os64_dom_animation_fire(f.binding,p2b_ms,&out),"frame: exception reported");
    p2b_script(&f,"assert(frames.join()==='error,after',frames.join());",
               "frame: exception leaves peers runnable");
    p2b_script(&f,
        "let ids=[];for(let i=0;i<1024;i++)ids.push(requestAnimationFrame(()=>{}));"
        "throws('QuotaExceededError',()=>requestAnimationFrame(()=>{}));"
        "ids.forEach(cancelAnimationFrame);assert(cancelAnimationFrame(-1)===undefined);"
        "requestAnimationFrame(()=>frames.push('pending at teardown'));",
        "frame: bounded queue, cancellation frees capacity, teardown owns pending callbacks");
    p2b_script(&f,"requestIdleCallback(d=>{globalThis.remaining=d.timeRemaining()})", "frame: idle grant setup");
    check(!os64_dom_idle_before(f.binding,p2b_ms,&out),"frame: no idle grant over due rendering");
    check(os64_dom_idle_before(f.binding,p2b_ms+7,&out),"frame: idle time before rendering");
    p2b_script(&f,"assert(remaining>0&&remaining<=7,String(remaining))","frame: idle deadline respects rendering");
    fixture_free(&f);

    f=fixture_new(&options);
    if(!fixture_ready(&f))return;
    dom_script(&f,"requestAnimationFrame(()=>{for(;;){}});requestAnimationFrame(()=>{globalThis.unreachable=true});",
               "frame: bounded batch setup");
    p2b_advance(20);dom_clock_step=1000;
    check(os64_dom_animation_fire(f.binding,p2b_ms,&out)&&out.status==OS64_JS_LIMIT,
          "frame: runaway callback exhausts batch budget");
    dom_clock_step=0;
    fixture_free(&f);

    options.alert=animation_spend;
    f=fixture_new(&options);
    if(!fixture_ready(&f))return;
    dom_script(&f,"requestAnimationFrame(()=>alert('one'))", "frame: single native callback budget");
    p2b_advance(20);
    check(os64_dom_animation_fire(f.binding,p2b_ms,&out)&&out.status==OS64_JS_OK,
          "frame: 600 ms fits the 1000 ms task limit");
    dom_script(&f,"requestAnimationFrame(()=>alert('one'));requestAnimationFrame(()=>alert('two'))",
               "frame: aggregate native callback budget");
    p2b_advance(20);
    check(os64_dom_animation_fire(f.binding,p2b_ms,&out)&&out.status==OS64_JS_LIMIT,
          "frame: two individually legal callbacks share one deadline");
    fixture_free(&f);options.alert=NULL;

    /* Refuse the queue allocation itself after registration has warmed the
     * script-origin lookup. The refused callback must leave no retained entry. */
    f=fixture_new(&options);
    if(!fixture_ready(&f))return;
    p2b_script(&f,"globalThis.cb=()=>{};cancelAnimationFrame(requestAnimationFrame(cb));",
               "frame: allocation refusal setup");
    dom_fail_at=dom_attempts+1;
    const char *source="requestAnimationFrame(cb)";
    check(os64_js_run(f.runtime,source,strlen(source),"frame-oom.js",&out)!=OS64_JS_OK,
          "frame: allocation refused");
    dom_fail_at=0;
    check(os64_dom_animation_next(f.binding)==UINT64_MAX,"frame: refusal leaves queue empty");
    fixture_free(&f);
}
