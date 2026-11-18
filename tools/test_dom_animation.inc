/* Frame snapshots, cancellation at checkpoints, shared timestamps and bounded
 * execution: assertions use visible callback behavior, not queue internals. */
static void animation_spend(void *opaque,const char *text,size_t length)
{
    (void)opaque;(void)text;(void)length;
    dom_clock+=600000;
}

static uint64_t animation_elapsed_ms;
static void animation_clock_spend(void *opaque,const char *text,size_t length)
{
    (void)opaque;(void)text;(void)length;
    p2b_advance(animation_elapsed_ms);
}
static void animation_deadlines(void)
{
    const uint64_t costs[]={0,6,40};
    for(unsigned i=0;i<sizeof(costs)/sizeof(*costs);++i) {
        p2b_ms=1000;p2b_us=1000000;animation_elapsed_ms=costs[i];
        os64_dom_options_t options=p2b_options(P2B_URL);options.alert=animation_clock_spend;
        DomFixture f=fixture_new(&options);if(!fixture_ready(&f))return;
        p2b_script(&f,"globalThis.n=0;function frame(){++n;alert('work');"
            "requestAnimationFrame(frame);Promise.resolve().then(()=>alert('checkpoint'))}"
            "requestAnimationFrame(frame)","frame pacing: work and checkpoint consume fake time");
        os64_js_outcome_t out;
        check(os64_dom_animation_next(f.binding)==1000,"frame pacing: a request is ready from when it was made");
        p2b_advance(17);
        check(os64_dom_animation_fire(f.binding,p2b_ms,OS64_DOM_FRAME_NOW,&out)&&out.status==OS64_JS_OK,
              "frame pacing: the host's frame runs the batch");
        p2b_script(&f,"assert(n===1,String(n))","frame pacing: nested request waits through checkpoint");
        uint64_t expected=1017+costs[i]*2;
        check(p2b_ms==expected&&os64_dom_animation_next(f.binding)==expected,
              "frame pacing: what the batch asked for is ready from its end, work and all");
        check(!os64_dom_animation_fire(f.binding,expected-1,OS64_DOM_FRAME_NOW,&out),
              "frame pacing: not before it was asked for");
        p2b_advance(17);
        check(os64_dom_animation_fire(f.binding,p2b_ms,OS64_DOM_FRAME_NOW,&out)&&out.status==OS64_JS_OK,
              "frame pacing: the next frame runs the next batch");
        p2b_script(&f,"assert(n===2,String(n))","frame pacing: exactly one successor runs");
        p2b_advance(3000);
        check(os64_dom_animation_fire(f.binding,p2b_ms,OS64_DOM_FRAME_NOW,&out)&&out.status==OS64_JS_OK,
              "frame pacing: delayed host resumes one batch");
        p2b_script(&f,"assert(n===3,String(n))","frame pacing: no catch-up callback burst");
        check(os64_dom_animation_next(f.binding)==p2b_ms,
              "frame pacing: resumed batch's successor is ready from its own end, not an old deadline");
        fixture_free(&f);
    }
}

static void dom_animation_cases(void)
{
    animation_deadlines();
    p2b_ms=1000;p2b_us=1000000;
    os64_dom_options_t options=p2b_options(P2B_URL);
    DomFixture f=fixture_new(&options);
    if(!fixture_ready(&f))return;
    os64_js_outcome_t out;
    check(os64_dom_animation_next(f.binding)==UINT64_MAX &&
          !os64_dom_animation_fire(f.binding,p2b_ms,OS64_DOM_FRAME_NOW,&out),"frame: empty queue sleeps");
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
    check(os64_dom_animation_next(f.binding)==1000,"frame: ready for the host's next frame");
    check(!os64_dom_animation_fire(f.binding,999,OS64_DOM_FRAME_NOW,&out),"frame: not before the request");
    p2b_advance(20);
    check(os64_dom_animation_fire(f.binding,p2b_ms,OS64_DOM_FRAME_NOW,&out)&&out.status==OS64_JS_OK,
          "frame: one batch with checkpoints");
    p2b_script(&f,"assert(frames.join()==='a,micro,b',frames.join());assert(times[0]===times[1]);",
               "frame: microtask cancellation, request order, same timestamp");
    check(os64_dom_animation_next(f.binding)==p2b_ms,
          "frame: a request made during the batch waits for the next frame, ready from the batch's end");
    p2b_advance(3000);
    check(os64_dom_animation_fire(f.binding,p2b_ms,OS64_DOM_FRAME_NOW,&out)&&out.status==OS64_JS_OK,
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
    check(os64_dom_animation_fire(f.binding,p2b_ms,OS64_DOM_FRAME_NOW,&out),"frame: exception reported");
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

    /* The frame's time is the host's: the batch's one timestamp is
     * performance.now() at the frame's instant, and an instant later than
     * the clock is taken as the clock, so no callback's argument passes the
     * performance.now() it reads. */
    f=fixture_new(&options);
    if(!fixture_ready(&f))return;
    p2b_script(&f,"globalThis.seen=[];requestAnimationFrame(t=>seen.push(t,performance.now()));"
               "requestAnimationFrame(t=>seen.push(t))","frame time: setup");
    p2b_advance(40);
    check(os64_dom_animation_fire(f.binding,p2b_ms,p2b_us-5000,&out)&&out.status==OS64_JS_OK,
          "frame time: a batch at the host's frame");
    p2b_script(&f,"assert(seen[0]===seen[2]&&seen[1]-seen[0]===5,String(seen))",
               "frame time: every callback gets the frame's time, 5 ms before performance.now()");
    p2b_script(&f,"requestAnimationFrame(t=>seen.push(t,performance.now()))","frame time: a later frame");
    p2b_advance(20);
    check(os64_dom_animation_fire(f.binding,p2b_ms,p2b_us+9000,&out)&&out.status==OS64_JS_OK,
          "frame time: a frame named past the clock");
    p2b_script(&f,"assert(seen[3]===seen[4]&&seen[3]>seen[2],String(seen))",
               "frame time: taken as the clock, never past performance.now()");
    fixture_free(&f);

    /* A frame asked for inside an idle period ends the grant at the
     * request; one asked for before it was the host's to bound (above). */
    animation_elapsed_ms=5;options.alert=animation_clock_spend;
    f=fixture_new(&options);
    if(!fixture_ready(&f))return;
    p2b_script(&f,"requestIdleCallback(d=>{alert('work');globalThis.before=d.timeRemaining();"
               "requestAnimationFrame(()=>{});globalThis.after=d.timeRemaining()})","idle frame: setup");
    check(os64_dom_idle_before(f.binding,UINT64_MAX,&out),"idle frame: a grant with no rendering due");
    p2b_script(&f,"assert(before>0&&after===0,before+' '+after)","idle frame: a frame asked for inside the grant ends it");
    fixture_free(&f);options.alert=NULL;

    f=fixture_new(&options);
    if(!fixture_ready(&f))return;
    dom_script(&f,"requestAnimationFrame(()=>{for(;;){}});requestAnimationFrame(()=>{globalThis.unreachable=true});",
               "frame: bounded batch setup");
    p2b_advance(20);dom_clock_step=1000;
    check(os64_dom_animation_fire(f.binding,p2b_ms,OS64_DOM_FRAME_NOW,&out)&&out.status==OS64_JS_LIMIT,
          "frame: runaway callback exhausts batch budget");
    dom_clock_step=0;
    fixture_free(&f);

    options.alert=animation_spend;
    f=fixture_new(&options);
    if(!fixture_ready(&f))return;
    dom_script(&f,"requestAnimationFrame(()=>alert('one'))", "frame: single native callback budget");
    p2b_advance(20);
    check(os64_dom_animation_fire(f.binding,p2b_ms,OS64_DOM_FRAME_NOW,&out)&&out.status==OS64_JS_OK,
          "frame: 600 ms fits the 1000 ms task limit");
    dom_script(&f,"requestAnimationFrame(()=>alert('one'));requestAnimationFrame(()=>alert('two'))",
               "frame: aggregate native callback budget");
    p2b_advance(20);
    check(os64_dom_animation_fire(f.binding,p2b_ms,OS64_DOM_FRAME_NOW,&out)&&out.status==OS64_JS_LIMIT,
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
