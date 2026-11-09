/* Web Animations (webanimation.c; WEB_ANIMATIONS.md): the Animation's
 * state machine, its promises and events through the update step, the
 * effects libflow is handed, and an animation nothing holds going with
 * the garbage. The values libflow draws from those effects are proved by
 * tools/test_libflow_script.inc. */

static const os64_html_node_t *wa_by_id(const os64_html_node_t *node, const char *id)
{
    for (; node != NULL; node = node->next) {
        const os64_html_attr_t *attr = node->kind == OS64_HTML_ELEMENT ? os64_html_attr(node, "id") : NULL;
        if (attr != NULL && strcmp(attr->value, id) == 0) return node;
        const os64_html_node_t *inside = wa_by_id(node->first_child, id);
        if (inside != NULL) return inside;
    }
    return NULL;
}

static bool wa_step(DomFixture *f, const char *what)
{
    os64_js_outcome_t out;
    bool ran = os64_dom_web_animations_update(f->binding, p2b_ms, &out);
    check(ran && out.status == OS64_JS_OK, what);
    return ran;
}

/* The sign-in page's key (WEB_ANIMATIONS.md § 1): animate, pause at once,
 * seek to 440 — the pause completes with the seek, and the effect libflow
 * reads holds there with both keyframes' easing. */
static void wa_x_key(void)
{
    p2b_ms = 1000; p2b_us = 1000000;
    os64_dom_options_t options = p2b_options(P2B_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    p2b_script(&f,
        "const d=document.getElementById('box');"
        "assert(typeof d.animate==='function'&&d.animate.length===1);"
        "for(const n of ['Animation','KeyframeEffect','DocumentTimeline','AnimationTimeline'])"
        "throws('TypeError',()=>new globalThis[n]());"
        "const a=d.animate({color:['#62df76','#31a6fd'],transform:['rotate(0deg)','rotate(165deg)'],"
        "easing:'cubic-bezier(0.23,-0.07,0.53,0.55)'},4096);"
        "assert(a instanceof Animation&&a instanceof EventTarget);"
        "assert(a.playState==='running'&&a.pending&&a.currentTime===0&&a.startTime===null,"
        "a.playState+' '+a.pending+' '+a.currentTime+' '+a.startTime);"
        "a.pause();assert(a.playState==='paused'&&a.pending);"
        "a.currentTime=440;"
        "assert(a.playState==='paused'&&!a.pending&&a.currentTime===440,a.playState+' '+a.pending+' '+a.currentTime);"
        "assert(a.effect instanceof KeyframeEffect&&a.effect.target===d&&a.effect===a.effect);"
        "assert(a.timeline===document.timeline&&document.timeline instanceof DocumentTimeline&&"
        "document.timeline instanceof AnimationTimeline);"
        "assert(a.id===''&&a.replaceState==='active'&&a.playbackRate===1);"
        "assert(d.getAnimations().length===1&&d.getAnimations()[0]===a&&document.getAnimations()[0]===a);"
        "assert(document.getElementById('one').getAnimations().length===0);"
        "assert(d.getAnimations({subtree:true}).length===1);"
        "globalThis.keep=a;",
        "web animations: the sign-in key's pause-and-seek");
    const os64_html_node_t *box = wa_by_id(f.document->document, "box");
    const struct garb_effect *const *fx = NULL;
    int32_t n = os64_dom_script_effects(f.binding, box, &fx);
    check(n == 1 && fx != NULL, "web animations: the box has one script effect");
    if (n == 1) {
        const garb_effect_t *e = fx[0];
        check(e->held && e->hold_ms == 440 && e->rate == 1, "web animations: the effect holds at 440");
        check(e->timing.duration_ms == 4096 && e->timing.iterations == 1 && e->timing.fill == GARB_FILL_NONE,
              "web animations: the effect's timing");
        check(e->nframes == 2 && e->frames[0].offset == 0 && e->frames[1].offset == 1,
              "web animations: two keyframes spaced 0 and 1");
        check(e->nframes == 2 && e->frames[0].has_easing && e->frames[0].easing.kind == GARB_EASE_CUBIC &&
              e->frames[0].easing.p[1] == -0.07 && e->frames[0].nsets == 2,
              "web animations: the keyframe's easing and its two longhands");
    }
    const os64_html_node_t *one = wa_by_id(f.document->document, "one");
    check(os64_dom_script_effects(f.binding, one, &fx) == 0 && fx == NULL,
          "web animations: an element no animation targets has none");
    p2b_script(&f,
        "const one=document.getElementById('one');"
        "one.animate({opacity:[0,0.5,1],easing:['ease-in','steps(2)']},10);one.animate({opacity:'0.5'},10);"
        "one.animate([{opacity:'calc(1',transform:'scale(2)'}],10);"
        "one.animate({opacity:[0,1,0,1],offset:[null,null,0.2]},10);one.animate([null,{opacity:1}],10);"
        "one.animate({opacity:[0,1,0,1,0],offset:[0,0.05]},10);one.animate([{opacity:'0\\u0000color:red'}],10);",
        "web animations: an easing list and a lone value");
    n = os64_dom_script_effects(f.binding, one, &fx);
    check(n == 7 && fx[0]->nframes == 3 && fx[0]->frames[0].easing.kind == GARB_EASE_CUBIC &&
          fx[0]->frames[1].easing.kind == GARB_EASE_STEPS && fx[0]->frames[2].easing.kind == GARB_EASE_CUBIC &&
          fx[0]->frames[1].offset == 0.5,
          "web animations: an easing list repeats over the keyframes, spaced evenly");
    check(n == 7 && fx[1]->nframes == 1 && fx[1]->frames[0].offset == 1,
          "web animations: a lone value is the keyframe at 1");
    /* Joined to its neighbour, `calc(1` would hold the `;` after it and
     * swallow the transform; read alone it closes at its end, as CSS does. */
    check(n == 7 && fx[2]->nframes == 1 && fx[2]->frames[0].nsets == 2,
          "web animations: an unclosed value takes no other with it");
    check(n == 7 && fx[3]->nframes == 4 && fx[3]->frames[1].offset == 0.1 && fx[3]->frames[2].offset == 0.2 &&
          fx[3]->frames[3].offset == 1,
          "web animations: an even placing is no offset: the missing ones are spaced round the given");
    check(n == 7 && fx[4]->nframes == 2 && fx[4]->frames[0].nsets == 0 && fx[4]->frames[0].offset == 0,
          "web animations: a null keyframe is an empty one");
    check(n == 7 && fx[5]->nframes == 5 && fabs(fx[5]->frames[2].offset - (0.05 + 0.95 / 3)) < 1e-9 &&
          fx[5]->frames[4].offset == 1,
          "web animations: the offsets left out are spaced from the given, not placed evenly");
    check(n == 7 && fx[6]->nframes == 1 && fx[6]->frames[0].nsets == 0,
          "web animations: a value holding a NUL is dropped whole, and declares nothing after it");
    fixture_free(&f);
}

/* A running animation through the update step: the pending play's task,
 * the timeline's time, finishing, and the order its promise and event come
 * in. */
static void wa_running(void)
{
    p2b_ms = 1000; p2b_us = 1000000;
    os64_dom_options_t options = p2b_options(P2B_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    check(os64_dom_web_animations_next(f.binding) == UINT64_MAX, "web animations: nothing to step");
    p2b_script(&f,
        "globalThis.log=[];const e=document.getElementById('one');"
        "globalThis.b=e.animate([{opacity:0},{opacity:1}],{duration:1000,fill:'forwards'});"
        "b.ready.then(x=>log.push('ready:'+(x===b)));b.finished.then(x=>log.push('finished:'+(x===b)));"
        "b.onfinish=ev=>log.push('finish:'+ev.currentTime+':'+ev.timelineTime+':'+"
        "(ev instanceof AnimationPlaybackEvent)+':'+(ev.target===b));"
        "globalThis.f=document.getElementById('two').animate({opacity:[0,1]},500);",
        "web animations: two animations made");
    check(os64_dom_web_animations_next(f.binding) == 0, "web animations: the first step is due at once");
    wa_step(&f, "web animations: the first step");
    p2b_script(&f,
        "assert(log.join()==='ready:true',log.join());"
        "assert(!b.pending&&b.startTime===0&&b.currentTime===0&&document.timeline.currentTime===0,"
        "b.startTime+' '+b.currentTime+' '+document.timeline.currentTime);",
        "web animations: the pending play ran at the step");
    check(os64_dom_web_animations_next(f.binding) == 1020, "web animations: the next step a frame on");
    const struct garb_effect *const *fx = NULL;
    check(os64_dom_script_effects(f.binding, wa_by_id(f.document->document, "one"), &fx) == 1 && !fx[0]->held &&
          fx[0]->start_ms == 1000 && fx[0]->rate == 1,
          "web animations: a running effect starts on the face's clock");
    p2b_advance(100);
    uint64_t version = os64_html_version(f.document);
    wa_step(&f, "web animations: a quiet step");
    check(os64_html_version(f.document) == version,
          "web animations: a step that only watched running animations lays nothing out again");
    p2b_advance(400);
    wa_step(&f, "web animations: a running step");
    p2b_script(&f,
        "assert(b.currentTime===500&&b.playState==='running',b.currentTime+' '+b.playState);"
        "assert(f.playState==='finished'&&f.currentTime===500,f.playState+' '+f.currentTime);"
        "assert(document.getElementById('two').getAnimations().length===0);"
        "assert(document.getAnimations().length===1);",
        "web animations: time moved with the timeline");
    p2b_advance(600);
    wa_step(&f, "web animations: the finishing step");
    check(os64_dom_web_animations_next(f.binding) == UINT64_MAX,
          "web animations: nothing running, nothing to step");
    p2b_script(&f,
        "assert(log.join()==='ready:true,finished:true,finish:1000:1100:true:true',log.join());"
        "assert(b.playState==='finished'&&b.currentTime===1000);"
        "assert(document.getElementById('one').getAnimations()[0]===b);",
        "web animations: finished, its promise before its event, and still in effect");
    /* Playing a finished animation rewinds it; reverse plays it back. */
    p2b_script(&f,
        "b.play();assert(b.pending&&b.currentTime===0,b.currentTime);"
        "b.reverse();assert(b.pending&&b.playbackRate===1&&b.currentTime===1000,b.currentTime);",
        "web animations: play rewinds, reverse from the start seeks to the end");
    p2b_advance(20);
    wa_step(&f, "web animations: reversed");
    p2b_script(&f,
        "assert(b.playbackRate===-1&&!b.pending&&b.playState==='running'&&b.currentTime===1000,"
        "b.playbackRate+' '+b.pending+' '+b.playState+' '+b.currentTime);",
        "web animations: the rate applied with the play task");
    p2b_advance(300);
    wa_step(&f, "web animations: running backwards");
    p2b_script(&f, "assert(b.currentTime===700,b.currentTime);", "web animations: backwards in time");
    p2b_script(&f, "b.finish();assert(Object.is(b.currentTime,0)&&b.playState==='finished',String(b.currentTime));",
               "web animations: finished backwards at 0, not -0");
    /* A finished animation turned round by its rate plays back, and a
     * pause of a paused one is nothing. */
    p2b_script(&f,
        "globalThis.u=document.getElementById('two').animate(null,1000);u.finish();u.updatePlaybackRate(-1);"
        "assert(u.playState==='running'&&u.currentTime===1000,u.playState+' '+u.currentTime);"
        "globalThis.q=document.getElementById('two').animate(null,1000);q.pause();",
        "web animations: a finished one reversed by its rate");
    p2b_advance(100);
    wa_step(&f, "web animations: playing back");
    p2b_script(&f,
        "assert(u.currentTime===900&&u.playState==='running',u.currentTime+' '+u.playState);"
        "const r=q.ready;assert(!q.pending);q.pause();assert(!q.pending&&q.ready===r,'pausing a paused one');",
        "web animations: it plays back from its end; pause of a paused one");
    /* Finish events in the order the ends were reached, not made. */
    p2b_script(&f,
        "globalThis.order=[];const two=document.getElementById('two');"
        "globalThis.A=two.animate(null,100000);globalThis.B=two.animate(null,100000);"
        "A.onfinish=()=>order.push('A');B.onfinish=()=>order.push('B');"
        "const z=two.animate(null,{duration:1000,delay:1000});z.playbackRate=0;"
        "assert(two.getAnimations().indexOf(z)<0,'a rate of 0 never reaches its active phase');",
        "web animations: two to finish");
    p2b_advance(20);
    wa_step(&f, "web animations: the two started");
    p2b_script(&f, "A.currentTime=99998;B.currentTime=99999;", "web animations: B's end comes first");
    p2b_advance(20);
    wa_step(&f, "web animations: both finish in one step");
    p2b_script(&f, "assert(order.join()==='B,A',order.join())", "web animations: sorted by when each ended");
    fixture_free(&f);
}

static void wa_procedures(void)
{
    p2b_ms = 1000; p2b_us = 1000000;
    os64_dom_options_t options = p2b_options(P2B_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    p2b_script(&f,
        "globalThis.clog=[];const two=document.getElementById('two');"
        "const c=two.animate({opacity:[0,1]},1000);"
        "c.ready.then(()=>clog.push('ready'),e=>clog.push('ready:'+e.name));c.finished.catch(e=>clog.push(e.name));"
        "c.oncancel=e=>clog.push('cancel:'+e.currentTime);"
        "c.cancel();assert(c.playState==='idle'&&c.currentTime===null&&c.startTime===null&&!c.pending);"
        "assert(two.getAnimations().length===0);"
        "c.cancel();",
        "web animations: cancel");
    const os64_html_node_t *two = wa_by_id(f.document->document, "two");
    const struct garb_effect *const *fx = NULL;
    check(os64_dom_script_effects(f.binding, two, &fx) == 0, "web animations: an idle animation has no effect");
    p2b_script(&f, "globalThis.beside=document.getElementById('box').animate({opacity:[0,1]},1000);",
               "web animations: one running beside the idle one");
    check(os64_dom_script_effects(f.binding, two, &fx) == 0 &&
          os64_dom_script_effects(f.binding, wa_by_id(f.document->document, "box"), &fx) == 1,
          "web animations: beside a running one, still none for the idle one");
    wa_step(&f, "web animations: the cancel step");
    p2b_script(&f, "assert(clog.join()==='ready:AbortError,AbortError,cancel:null',clog.join());",
               "web animations: cancel rejects both pending promises, fires once");
    p2b_script(&f,
        "const box=document.getElementById('box');"
        "const g=box.animate({opacity:[0,1]},200);globalThis.glog=[];g.finished.then(()=>glog.push('done'));"
        "g.finish();assert(g.playState==='finished'&&g.currentTime===200&&!g.pending);"
        "const r=box.animate({opacity:[0,1]},{duration:100,iterations:Infinity});"
        "throws('InvalidStateError',()=>r.reverse());assert(r.playbackRate===1);"
        "throws('InvalidStateError',()=>r.finish());"
        "const p=box.animate({opacity:[0,1]},1000);"
        "p.updatePlaybackRate(0.5);assert(p.playbackRate===1&&p.pending);"
        "globalThis.p=p;"
        "const z=box.animate({opacity:[0,1]},1000);z.playbackRate=0;"
        "throws('InvalidStateError',()=>z.finish());"
        "const q=box.animate({opacity:[0,1]},{duration:1000,delay:100,endDelay:50,id:'q'});"
        "assert(q.id==='q');q.id=7;assert(q.id==='7');"
        "q.currentTime=600;assert(q.currentTime===600);"
        "throws('TypeError',()=>{q.currentTime=null});throws('TypeError',()=>{q.currentTime=NaN});"
        "q.startTime=null;"
        "const ev=new AnimationPlaybackEvent('finish',{currentTime:5,timelineTime:null});"
        "assert(ev.type==='finish'&&ev.currentTime===5&&ev.timelineTime===null&&ev instanceof Event);"
        "const bare=new AnimationPlaybackEvent('cancel');assert(bare.currentTime===null&&bare.timelineTime===null);",
        "web animations: finish, reverse, rates, seeks and the event's constructor");
    p2b_script(&f,
        "const box=document.getElementById('box');"
        /* finish() resolves the finished promise there and then */
        "globalThis.s1=box.animate({opacity:[0,1]},100);globalThis.syncOrder=[];let res=false;s1.finish();"
        "s1.finished.then(()=>{res=true});Promise.resolve().then(()=>syncOrder.push(res));"
        /* a finish seeked away before it is heard is not heard */
        "globalThis.s2=box.animate({opacity:[0,1]},100);globalThis.s2fired=false;"
        "s2.finished.then(()=>{s2fired=true});s2.onfinish=()=>{s2fired=true};s2.currentTime=100;s2.currentTime=50;"
        /* falling out of finished makes a new finished promise */
        "const s3=box.animate({opacity:[0,1]},100);s3.finish();const f3=s3.finished;s3.currentTime=0;"
        "assert(s3.finished!==f3,'a new finished promise');"
        /* a seek past the end, then a rate: the time it was set to stays */
        "globalThis.s4=box.animate({opacity:[0,1]},100);s4.currentTime=150;",
        "web animations: finishing, and falling out of it");
    p2b_advance(20);
    wa_step(&f, "web animations: the rate step");
    p2b_script(&f,
        "assert(syncOrder[0]===true,'finish() resolves at once');assert(!s2fired,'no finish for one seeked away');"
        "s4.playbackRate=2;assert(s4.currentTime===150&&s4.playState==='finished',s4.currentTime+' '+s4.playState);",
        "web animations: what finishing promised");
    p2b_script(&f, "assert(glog.join()==='done'&&p.playbackRate===0.5&&!p.pending,glog.join()+' '+p.playbackRate);",
               "web animations: a finished promise resolved, a pending rate applied");
    p2b_script(&f,
        "const box=document.getElementById('box');"
        "throws('TypeError',()=>box.animate([{offset:0.6,opacity:0},{offset:0.2,opacity:1}],1000));"
        "throws('TypeError',()=>box.animate([{offset:2,opacity:0}],1000));"
        "throws('TypeError',()=>box.animate({opacity:[0,1]},-1));"
        "throws('TypeError',()=>box.animate({opacity:[0,1]},{duration:'long'}));"
        "throws('TypeError',()=>box.animate({opacity:[0,1]},{iterations:-1}));"
        "throws('TypeError',()=>box.animate({opacity:[0,1]},{easing:'bogus'}));"
        "throws('TypeError',()=>box.animate({opacity:[0,1]},{fill:'sideways'}));"
        "throws('TypeError',()=>box.animate({opacity:[0,1],easing:'bogus'},10));"
        "throws('NotSupportedError',()=>box.animate({opacity:[0,1]},{pseudoElement:'::before'}));"
        "for(const p of ['','before',':abc','::abc','::marker()',':marker'])"
        "throws('SyntaxError',()=>box.animate(null,{pseudoElement:p}));"
        "for(const e of ['','7','inherit','initial','ease-in-out, ease-out','cubic-bezier(1.1, 0, 1, 1)',"
        "'cubic-bezier(0, 0, -0.1, 1)','steps(-1, start)','function (x){return x}']){"
        "throws('TypeError',()=>box.animate({easing:e},10));throws('TypeError',()=>box.animate({easing:['linear',e]},10));"
        "throws('TypeError',()=>box.animate([{opacity:0,easing:e}],10));throws('TypeError',()=>box.animate(null,{easing:e}))}"
        "const listed=box.animate({opacity:[0,0.5,1],easing:['ease-in','steps(2)']},10);assert(listed.playState==='running');"
        "throws('TypeError',()=>box.animate([{opacity:0,composite:'plus'}],10));"
        "throws('TypeError',()=>box.animate({opacity:[0,1],composite:['add','plus']},10));"
        "throws('TypeError',()=>box.animate(null,{composite:'auto'}));"
        "throws('TypeError',()=>box.animate(null,{iterationComposite:'add'}));"
        "box.animate([{opacity:0,composite:'auto'},{opacity:1,composite:'add'}],{composite:'add',iterationComposite:'accumulate'});"
        "throws('TypeError',()=>Element.prototype.animate.call(document,{},1));"
        "const empty=box.animate(null,10);assert(empty.playState==='running');"
        "const odd=box.animate({notAProperty:[1,2],opacity:['0.5']},10);assert(odd.effect.target===box);"
        "const seen=[],o={};for(const k of ['id','pseudoElement','iterations','iterationStart','fill','endDelay',"
        "'easing','duration','direction','delay'])Object.defineProperty(o,k,{get(){seen.push(k)}});"
        "const kf={get opacity(){seen.push('keyframes');return [0,1]}};box.animate(kf,{get duration(){seen.push('options');return 1}});"
        "assert(seen.join()==='options,keyframes',seen.join());seen.length=0;"
        "box.animate([{get width(){seen.push('width')},get color(){seen.push('color')},get offset(){seen.push('offset')},"
        "get easing(){seen.push('easing')},get composite(){seen.push('composite')}}],1);"
        "box.animate({get width(){seen.push('w')},get offset(){seen.push('o')},get color(){seen.push('c')}},1);"
        "assert(seen.join()==='composite,easing,offset,color,width,o,c,w',seen.join());seen.length=0;"
        "box.animate(null,o);assert(seen.join()==='delay,direction,duration,easing,endDelay,fill,iterationStart,"
        "iterations,pseudoElement,id',seen.join());",
        "web animations: refused arguments");
    fixture_free(&f);
}

/* An animation nothing holds is let go once it can change nothing more,
 * and its keyframes outlive it until the host releases their generation. */
static void wa_collection(void)
{
    p2b_ms = 1000; p2b_us = 1000000;
    os64_dom_options_t options = p2b_options(P2B_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    const os64_html_node_t *box = wa_by_id(f.document->document, "box");
    const struct garb_effect *const *fx = NULL;
    p2b_script(&f, "document.getElementById('box').animate({opacity:[0,1]},100);",
               "web animations: an animation nobody keeps");
    check(os64_dom_script_effects(f.binding, box, &fx) == 1, "web animations: it is drawn while it runs");
    uint64_t generation = os64_dom_effects_mark(f.binding);
    wa_step(&f, "web animations: started");
    os64_js_outcome_t gc;
    JS_RunGC(JS_GetRuntime(os64_js_context(f.runtime, OS64_JS_ABI_ID, &gc)));
    check(os64_dom_script_effects(f.binding, box, &fx) == 1, "web animations: the binding keeps it running");
    p2b_advance(200);
    wa_step(&f, "web animations: finished");
    size_t before = os64_dom_bytes(f.binding);
    JS_RunGC(JS_GetRuntime(os64_js_context(f.runtime, OS64_JS_ABI_ID, &gc)));
    check(os64_dom_script_effects(f.binding, box, &fx) == 0, "web animations: collected once it is done");
    check(os64_dom_bytes(f.binding) <= before, "web animations: collecting frees nothing a tree may read");
    size_t buried = os64_dom_bytes(f.binding);
    os64_dom_effects_release(f.binding, generation);
    check(os64_dom_bytes(f.binding) == buried, "web animations: a tree as old as its death still holds it");
    os64_dom_effects_release(f.binding, os64_dom_effects_mark(f.binding));
    check(os64_dom_bytes(f.binding) < buried, "web animations: released with the last tree that could read it");
    /* One nobody keeps, reversed into its delay, where it is not relevant:
     * still running to its finish, still held, and its listener hears it. */
    p2b_script(&f,
        "(function(){var r=document.getElementById('box').animate({opacity:[0,1]},{duration:1000,delay:50});"
        "r.onfinish=function(){globalThis.rfin=String(r.currentTime)};"
        "r.ready.then(function(){r.currentTime=100;r.reverse()})})()",
        "web animations: reversed into its delay, unreferenced");
    for (int i = 0; i < 8; i++) {
        p2b_advance(i == 0 ? 20 : 60);
        /* Due while it runs; none once it has finished. */
        if (os64_dom_web_animations_next(f.binding) <= p2b_ms)
            wa_step(&f, "web animations: a step of the reversed one");
        JS_RunGC(JS_GetRuntime(os64_js_context(f.runtime, OS64_JS_ABI_ID, &gc)));
    }
    p2b_script(&f, "assert(globalThis.rfin==='0',String(globalThis.rfin))",
               "web animations: it finished at 0, heard through a collection");
    /* One a script holds at teardown, mid-run, is cut loose by the drain. */
    p2b_script(&f, "globalThis.held=document.getElementById('box').animate({opacity:[0,1]},100000);",
               "web animations: held at teardown");
    fixture_free(&f);
}

static void dom_webanimation_cases(void)
{
    wa_x_key();
    wa_running();
    wa_procedures();
    wa_collection();
}
