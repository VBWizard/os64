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
        "for(const n of ['KeyframeEffect','DocumentTimeline','AnimationTimeline','AnimationEffect'])"
        "throws('TypeError',()=>new globalThis[n]());"
        "const idle=new Animation();"
        "assert(idle.playState==='idle'&&idle.effect===null&&idle.timeline===document.timeline,"
        "'new Animation(): '+idle.playState+' '+idle.effect);"
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
        "one.animate({opacity:[0,1,0,1,0],offset:[0,0.05]},10);one.animate([{opacity:'0\\u0000color:red'}],10);"
        "one.animate([{borderTop:'10px solid red',borderWidth:'1px',borderRightWidth:'5px'}],10);",
        "web animations: an easing list and a lone value");
    n = os64_dom_script_effects(f.binding, one, &fx);
    check(n == 8 && fx[0]->nframes == 3 && fx[0]->frames[0].easing.kind == GARB_EASE_CUBIC &&
          fx[0]->frames[1].easing.kind == GARB_EASE_STEPS && fx[0]->frames[2].easing.kind == GARB_EASE_CUBIC &&
          fx[0]->frames[1].offset == 0.5,
          "web animations: an easing list repeats over the keyframes, spaced evenly");
    check(n == 8 && fx[1]->nframes == 1 && fx[1]->frames[0].offset == 1,
          "web animations: a lone value is the keyframe at 1");
    /* Joined to its neighbour, `calc(1` would hold the `;` after it and
     * swallow the transform; read alone it closes at its end, as CSS does. */
    check(n == 8 && fx[2]->nframes == 1 && fx[2]->frames[0].nsets == 2,
          "web animations: an unclosed value takes no other with it");
    check(n == 8 && fx[3]->nframes == 4 && fx[3]->frames[1].offset == 0.1 && fx[3]->frames[2].offset == 0.2 &&
          fx[3]->frames[3].offset == 1,
          "web animations: an even placing is no offset: the missing ones are spaced round the given");
    check(n == 8 && fx[4]->nframes == 2 && fx[4]->frames[0].nsets == 0 && fx[4]->frames[0].offset == 0,
          "web animations: a null keyframe is an empty one");
    check(n == 8 && fx[5]->nframes == 5 && fabs(fx[5]->frames[2].offset - (0.05 + 0.95 / 3)) < 1e-9 &&
          fx[5]->frames[4].offset == 1,
          "web animations: the offsets left out are spaced from the given, not placed evenly");
    check(n == 8 && fx[6]->nframes == 1 && fx[6]->frames[0].nsets == 0,
          "web animations: a value holding a NUL is dropped whole, and declares nothing after it");
    double widths[4] = {-1, -1, -1, -1};
    for (int32_t i = 0; n == 8 && i < fx[7]->frames[0].nsets; i++) {
        const garb_set_t *set = &fx[7]->frames[0].sets[i];
        if (set->prop >= GARB_BORDER_TOP_WIDTH && set->prop <= GARB_BORDER_LEFT_WIDTH)
            widths[set->prop - GARB_BORDER_TOP_WIDTH] = set->value.number;
    }
    check(n == 8 && widths[0] == 10 && widths[1] == 5 && widths[2] == 1 && widths[3] == 1,
          "web animations: of a keyframe's declarations, the one setting fewer longhands wins each");
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
        "c.finished.then=null;c.ready.then=function(){globalThis.reentered=true};"
        "c.cancel();assert(c.playState==='idle'&&c.currentTime===null&&c.startTime===null&&!c.pending);"
        "assert(!globalThis.reentered,'cancel runs no then of the page');"
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
        "box.animate([{get marginTop(){seen.push('marginTop')},get opacity(){seen.push('opacity')},"
        "get margin(){seen.push('margin')}}],1);"
        "assert(seen.join()==='margin,marginTop,opacity',seen.join());seen.length=0;"
        /* the page's own members are never read, a symbol among them */
        "box.animate([{opacity:0,metadata:Symbol(),get data(){seen.push('data')}},{opacity:1}],1);"
        "box.animate({opacity:[0,1],note:Symbol(),get extra(){seen.push('extra');return [1,2,3]}},1);"
        "assert(seen.length===0,seen.join());"
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

/* A frame callback reads the timeline at its own timestamp, nothing
 * stepping meanwhile (HTML gives both one time). */
static void wa_frame_time(void)
{
    p2b_ms = 1000; p2b_us = 1000000;
    os64_dom_options_t options = p2b_options(P2B_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    p2b_script(&f,
        "const a=document.getElementById('box').animate(null,10);a.pause();a.currentTime=5;"
        "globalThis.early=document.timeline.currentTime;globalThis.tl='none';"
        "requestAnimationFrame(t=>{tl=document.timeline.currentTime===t&&t>early?'same':t+' '+document.timeline.currentTime})",
        "web animations: a frame callback asked for");
    p2b_advance(50);
    os64_js_outcome_t out;
    check(os64_dom_animation_fire(f.binding, p2b_ms, &out) && out.status == OS64_JS_OK,
          "web animations: the frame callback runs");
    p2b_script(&f, "assert(tl==='same',String(tl))", "web animations: the timeline's time is the frame's");
    fixture_free(&f);
}

/* KeyframeEffect as an object of its own (slice B): made, copied,
 * read back as the page gave it, its timing updated all or nothing, moved
 * between animations, and its target and keyframes changed under the
 * layouts. */
static void wa_effects(void)
{
    p2b_ms = 1000; p2b_us = 1000000;
    os64_dom_options_t options = p2b_options(P2B_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    const os64_html_node_t *box = wa_by_id(f.document->document, "box");
    const os64_html_node_t *one = wa_by_id(f.document->document, "one");
    const struct garb_effect *const *fx = NULL;
    p2b_script(&f,
        "const d=document.getElementById('box'),one=document.getElementById('one');"
        "const e=new KeyframeEffect(d,[{opacity:0,easing:'ease-in',composite:'add'},{opacity:0.5,offset:0.4},"
        "{opacity:1,backgroundColor:'red',cssFloat:'left'}],{duration:1000,fill:'forwards',easing:'steps(2, start)'});"
        "assert(e instanceof KeyframeEffect&&e instanceof AnimationEffect&&e.target===d&&KeyframeEffect.length===1);"
        "const k=e.getKeyframes();"
        "assert(k.length===3&&k[0].offset===null&&k[0].computedOffset===0&&k[0].easing==='ease-in'&&"
        "k[0].composite==='add'&&k[0].opacity==='0',JSON.stringify(k[0]));"
        "assert(k[1].offset===0.4&&k[1].computedOffset===0.4&&k[1].easing==='linear'&&k[1].composite==='auto',"
        "JSON.stringify(k[1]));"
        "assert(k[2].offset===null&&k[2].computedOffset===1&&k[2].backgroundColor==='red'&&k[2].cssFloat==='left',"
        "JSON.stringify(k[2]));"
        "const t=e.getTiming();"
        "assert(t.duration===1000&&t.fill==='forwards'&&t.easing==='steps(2, start)'&&t.delay===0&&"
        "t.iterations===1&&t.direction==='normal'&&t.endDelay===0&&t.iterationStart===0,JSON.stringify(t));"
        "const plain=new KeyframeEffect(null,null);"
        "assert(new KeyframeEffect(null,null,500).getTiming().duration===500,'a number is the duration');"
        "const dt=plain.getTiming();assert(dt.fill==='auto'&&dt.duration==='auto'&&dt.easing==='linear',JSON.stringify(dt));"
        "const ct=plain.getComputedTiming();"
        "assert(ct.fill==='none'&&ct.duration===0&&ct.localTime===null&&ct.progress===null&&"
        "ct.currentIteration===null&&ct.endTime===0&&ct.activeDuration===0,JSON.stringify(ct));"
        "assert(plain.target===null&&plain.getKeyframes().length===0&&plain.pseudoElement===null);"
        "const kv=new KeyframeEffect(null,{left:['10px','20px'],top:['15px','bogus'],'--custom':['a','b']}).getKeyframes();"
        "assert(kv[1].left==='20px'&&!('top' in kv[1])&&kv[0].top==='15px'&&kv[0]['--custom']==='a',JSON.stringify(kv));"
        "throws('TypeError',()=>plain.setKeyframes());"
        "assert(plain.getComputedTiming().startTime===0);"
        "e.updateTiming({delay:100,iterations:2});"
        "const t2=e.getTiming();assert(t2.delay===100&&t2.iterations===2&&t2.duration===1000&&t2.fill==='forwards');"
        "throws('TypeError',()=>e.updateTiming({delay:5,duration:-1}));"
        "assert(e.getTiming().delay===100&&e.getTiming().duration===1000,'a throw keeps all');"
        "e.updateTiming({fill:'auto',duration:'auto'});"
        "assert(e.getTiming().fill==='auto'&&e.getTiming().duration==='auto'&&e.getComputedTiming().duration===0);"
        "e.updateTiming({fill:'forwards',duration:1000});"
        "throws('TypeError',()=>new KeyframeEffect(e,{opacity:[0,1]}));"
        "class Fade extends Animation{stop(){this.cancel();return 'ok'}}"
        "class Kf extends KeyframeEffect{}"
        "assert(new Fade().stop()==='ok'&&new Kf(null,null) instanceof Kf&&new Kf(new Kf(null,null)) instanceof Kf,"
        "'subclasses keep their prototypes');"
        "const copy=new KeyframeEffect(e);"
        "assert(copy!==e&&copy.target===d&&copy.getKeyframes().length===3&&copy.getTiming().delay===100&&"
        "copy.getKeyframes()[0].composite==='add');"
        "e.composite='accumulate';assert(e.composite==='accumulate');"
        "throws('TypeError',()=>{e.composite='bogus'});"
        "throws('NotSupportedError',()=>{e.pseudoElement='::before'});"
        "throws('SyntaxError',()=>{e.pseudoElement='nope'});"
        "const a=new Animation(e);assert(a.effect===e&&a.playState==='idle');"
        "a.play();assert(a.playState==='running'&&a.pending);"
        "globalThis.a=a;globalThis.e=e;",
        "web animations: KeyframeEffect made, read, copied and timed");
    check(os64_dom_script_effects(f.binding, box, &fx) == 1 && fx[0]->nframes == 3,
          "web animations: a made effect is drawn by its animation");
    p2b_script(&f,
        "const b=new Animation();b.effect=e;"
        "assert(b.effect===e&&a.effect===null,'moved: '+(a.effect===null));"
        "assert(a.playState==='finished'||a.playState==='running',a.playState);"
        "globalThis.b=b;",
        "web animations: an effect moves to the animation it is given to");
    check(os64_dom_script_effects(f.binding, box, &fx) == 0,
          "web animations: an idle animation's effect is not drawn");
    p2b_script(&f,
        "b.pause();b.currentTime=150;"
        "const c=e.getComputedTiming();"
        "assert(c.localTime===150&&c.progress===0.5&&c.currentIteration===0&&c.endTime===2100&&"
        "c.activeDuration===2000,JSON.stringify(c));"
        "e.setKeyframes({opacity:[0.2,0.8]});"
        "assert(e.getKeyframes().length===2&&e.getKeyframes()[0].opacity==='0.2');"
        "assert(e.getTiming().delay===100&&e.getTiming().duration===1000,'new keyframes keep the timing');",
        "web animations: computed timing and new keyframes");
    check(os64_dom_script_effects(f.binding, box, &fx) == 1 && fx[0]->nframes == 2,
          "web animations: the new keyframes are drawn");
    p2b_script(&f, "e.target=document.getElementById('one');", "web animations: a new target");
    check(os64_dom_script_effects(f.binding, box, &fx) == 0, "web animations: the old target is let go");
    check(os64_dom_script_effects(f.binding, one, &fx) == 1 && fx[0]->nframes == 2 && fx[0]->held &&
              fx[0]->hold_ms == 150,
          "web animations: the new target draws the new keyframes at the held time");
    fixture_free(&f);
}

/* § 5.5: finished, filling animations a later one covers are removed and
 * hear `remove`; a persisted one is not; commitStyles writes the computed
 * value into the style attribute. */
static void wa_replace(void)
{
    p2b_ms = 1000; p2b_us = 1000000;
    os64_dom_options_t options = p2b_options(P2B_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    ComputedFake c;
    memset(&c, 0, sizeof(c));
    os64_dom_set_computed(f.binding, fake_computed, &c);
    const os64_html_node_t *box = wa_by_id(f.document->document, "box");
    const struct garb_effect *const *fx = NULL;
    p2b_script(&f,
        "const d=document.getElementById('box');globalThis.heard=[];"
        "const o={duration:10,fill:'forwards'};"
        "globalThis.o1=document.getElementById('one').animate({color:['red','blue']},o);"
        "globalThis.a1=d.animate({opacity:[0,1]},o);globalThis.a2=d.animate({opacity:[1,0]},o);"
        "globalThis.a3=d.animate({color:['red','blue']},o);"
        "globalThis.p1=d.animate({width:['1px','2px']},o);p1.persist();globalThis.p2=d.animate({width:['2px','3px']},o);"
        "for(const n of ['o1','a1','a2','a3','p1','p2'])globalThis[n].onremove=ev=>heard.push(n+':'+ev.type+':'+"
        "(ev instanceof AnimationPlaybackEvent));"
        "assert(p1.replaceState==='persisted'&&a1.replaceState==='active');",
        "web animations: five filling animations");
    wa_step(&f, "web animations: started");
    p2b_script(&f, "assert(heard.length===0,heard.join())", "web animations: nothing running is replaced");
    p2b_advance(50);
    wa_step(&f, "web animations: finished, and the replaced removed");
    p2b_script(&f,
        "assert(heard.join()==='a1:remove:true',heard.join());"
        "assert(a1.replaceState==='removed'&&a2.replaceState==='active'&&a3.replaceState==='active'&&"
        "p1.replaceState==='persisted',a1.replaceState);"
        "assert(document.getElementById('box').getAnimations().indexOf(a1)<0&&"
        "document.getElementById('box').getAnimations().length===4);",
        "web animations: the covered one went, the persisted one stayed");
    check(os64_dom_script_effects(f.binding, box, &fx) == 4, "web animations: a removed effect is not drawn");
    p2b_script(&f, "a1.persist();assert(a1.replaceState==='persisted');", "web animations: persisted after removal");
    check(os64_dom_script_effects(f.binding, box, &fx) == 5, "web animations: a persisted one is drawn again");
    p2b_script(&f,
        "const o={duration:10,fill:'forwards'};"
        "globalThis.x1=document.getElementById('one').animate({left:['1px','2px']},o);"
        "globalThis.x2=document.getElementById('box').animate({left:['2px','3px']},o);",
        "web animations: two filling animations of two targets");
    p2b_advance(20);
    wa_step(&f, "web animations: those started");
    p2b_advance(50);
    wa_step(&f, "web animations: those finished");
    p2b_script(&f, "assert(x1.replaceState==='active'&&x2.replaceState==='active');"
                   "x2.effect.target=document.getElementById('one');",
               "web animations: neither replaces the other, until one moves to the other's target");
    p2b_advance(20);
    check(os64_dom_web_animations_next(f.binding) <= p2b_ms, "web animations: a moved effect asks for a step");
    wa_step(&f, "web animations: the step after the move");
    p2b_script(&f, "assert(x1.replaceState==='removed',x1.replaceState)", "web animations: the moved one replaces");
    p2b_script(&f,
        "const d=document.getElementById('box');"
        "a3.commitStyles();"
        "assert((d.getAttribute('style')||'').indexOf('color:rgb(1, 2, 3)')>=0,String(d.getAttribute('style')));"
        "throws('NoModificationAllowedError',()=>new Animation(new KeyframeEffect(null,{opacity:[0,1]},10)).commitStyles());"
        "globalThis.det=document.createElement('div').animate({color:['red','blue']},10);"
        "throws('InvalidStateError',()=>det.commitStyles());"
        "globalThis.nt=new Animation(new KeyframeEffect(d,{opacity:[0,1]},100),null);nt.play();"
        "assert(nt.timeline===null&&nt.pending);",
        "web animations: commitStyles writes the computed value");
    c.body_display = "none";
    p2b_script(&f, "throws('InvalidStateError',()=>a3.commitStyles())",
               "web animations: commitStyles refuses a target under display: none");
    c.body_display = "contents";
    p2b_script(&f, "throws('InvalidStateError',()=>document.body.animate({color:['red','blue']},10).commitStyles())",
               "web animations: and a target that is display: contents");
    c.body_display = NULL;
    p2b_script(&f,
        "(function(){const n=new Animation(new KeyframeEffect(document.getElementById('box'),{opacity:[0,1]},1000),null);"
        "n.currentTime=500;n.id='kept'})();"
        "const loose=document.createElement('div');globalThis.loose=loose;"
        "globalThis.l1=loose.animate({opacity:[0,1]},{duration:10,fill:'forwards'});"
        "globalThis.l2=loose.animate({opacity:[1,0]},{duration:10,fill:'forwards'});",
        "web animations: a positioned timeline-less one nobody keeps, and two on a detached element");
    p2b_advance(20);
    wa_step(&f, "web animations: a step with an animation that has no timeline");
    p2b_advance(50);
    wa_step(&f, "web animations: the detached ones finished");
    os64_js_outcome_t gc2;
    JS_RunGC(JS_GetRuntime(os64_js_context(f.runtime, OS64_JS_ABI_ID, &gc2)));
    p2b_script(&f,
        "assert(document.getAnimations().some(x=>x.id==='kept'),'a timeline-less one that shows is kept');"
        "assert(l1.replaceState==='active'&&l2.replaceState==='active','nothing detached is replaced');"
        "l1.cancel();l2.cancel();",
        "web animations: kept while it shows, and detached ones left alone");
    p2b_script(&f, "assert(nt.pending&&nt.startTime===null&&nt.currentTime===0,nt.pending+' '+nt.startTime);"
                   "for(const x of document.getAnimations())if(x!==nt)x.cancel();det.cancel()",
               "web animations: one with no timeline never starts");
    p2b_advance(20);
    if (os64_dom_web_animations_next(f.binding) <= p2b_ms) wa_step(&f, "web animations: the cancels' events");
    p2b_advance(20);
    check(os64_dom_web_animations_next(f.binding) == UINT64_MAX,
          "web animations: one waiting for a time it will not have asks for no steps");
    fixture_free(&f);
}

/* Composite (slice B2): each keyframe hands libflow its own composite, or
 * its effect's where it said auto, and the effect its iterationComposite;
 * changing the effect's composite changes what an auto keyframe hands. */
static void wa_composite(void)
{
    p2b_ms = 1000; p2b_us = 1000000;
    os64_dom_options_t options = p2b_options(P2B_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    const os64_html_node_t *box = wa_by_id(f.document->document, "box");
    const struct garb_effect *const *fx = NULL;
    p2b_script(&f,
        "globalThis.ca=document.getElementById('box').animate([{marginLeft:'0px'},{marginLeft:'10px',composite:'replace'}],"
        "{duration:1000,composite:'add',iterationComposite:'accumulate'});ca.pause();",
        "web animations: an effect that adds, one keyframe replacing");
    check(os64_dom_script_effects(f.binding, box, &fx) == 1 && fx[0]->frames[0].composite == GARB_COMPOSITE_ADD &&
              fx[0]->frames[1].composite == GARB_COMPOSITE_REPLACE && fx[0]->iteration_accumulate,
          "web animations: auto takes the effect's, its own stays");
    p2b_script(&f, "ca.effect.composite='accumulate';ca.effect.iterationComposite='replace';",
               "web animations: the effect's composite changed");
    check(os64_dom_script_effects(f.binding, box, &fx) == 1 &&
              fx[0]->frames[0].composite == GARB_COMPOSITE_ACCUMULATE &&
              fx[0]->frames[1].composite == GARB_COMPOSITE_REPLACE && !fx[0]->iteration_accumulate,
          "web animations: and an auto keyframe follows it");
    p2b_script(&f, "ca.effect.setKeyframes([{marginLeft:'0px'},{marginLeft:'10px'}]);",
               "web animations: new keyframes, both auto");
    check(os64_dom_script_effects(f.binding, box, &fx) == 1 &&
              fx[0]->frames[1].composite == GARB_COMPOSITE_ACCUMULATE,
          "web animations: new keyframes take the effect's");
    fixture_free(&f);
}

/* A host's CSS animations (os64_dom_set_css_animations), as libflow's
 * timeline would answer: a table the cases write, whose drive records
 * what a CSSAnimation handed it. */
typedef struct {
    os64_dom_css_animation_t r;
    bool scripted;
} WaCssRecord;
static WaCssRecord wa_css_records[8];
static int32_t wa_css_n, wa_css_counted_current;
static uint64_t wa_css_version;

static int32_t wa_css_count(void *opaque, bool current)
{
    (void)opaque;
    wa_css_counted_current += current;
    return wa_css_n;
}

static bool wa_css_at(void *opaque, int32_t i, os64_dom_css_animation_t *out)
{
    (void)opaque;
    if (i < 0 || i >= wa_css_n) return false;
    *out = wa_css_records[i].r;
    return true;
}

static bool wa_css_drive(void *opaque, uint64_t id, bool paused, bool canceled, double start, double hold, double rate)
{
    (void)opaque;
    for (int32_t i = 0; i < wa_css_n; i++) {
        os64_dom_css_animation_t *r = &wa_css_records[i].r;
        if (r->id != id) continue;
        wa_css_records[i].scripted = true;
        r->paused = paused;
        r->canceled = canceled;
        r->start_ms = start;
        r->hold_ms = hold;
        r->rate = rate;
        return true;
    }
    return false;
}

static uint64_t wa_css_version_of(void *opaque)
{
    (void)opaque;
    return wa_css_version;
}

static WaCssRecord *wa_css_add(uint64_t id, const os64_html_node_t *element, const char *name, double start,
                               double duration, double delay, double iterations)
{
    WaCssRecord *c = &wa_css_records[wa_css_n++];
    memset(c, 0, sizeof(*c));
    c->r = (os64_dom_css_animation_t){.id = id, .element = element, .name = name, .name_len = strlen(name),
                                      .start_ms = start, .rate = 1, .duration_ms = duration, .delay_ms = delay,
                                      .iterations = iterations, .direction = GARB_DIRECTION_NORMAL,
                                      .fill = GARB_FILL_NONE};
    wa_css_version++;
    return c;
}

static void wa_css_install(DomFixture *f)
{
    wa_css_n = 0;
    wa_css_counted_current = 0;
    os64_dom_css_animations_t css = {wa_css_count, wa_css_at, wa_css_drive, wa_css_version_of, NULL};
    os64_dom_set_css_animations(f->binding, &css);
}

/* CSSAnimation (WEB_ANIMATIONS.md § 3): getAnimations answers the host's
 * CSS animations first, each the same object while it lives; the
 * CSSAnimation's state machine drives the host's clock from then on —
 * pause, rate, cancel — and animation-play-state's changes reach it as a
 * pause or a play; one whose CSS animation went is canceled. */
static void wa_css(void)
{
    p2b_ms = 1000; p2b_us = 1000000;
    os64_dom_options_t options = p2b_options(P2B_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    wa_css_install(&f);
    const os64_html_node_t *one = wa_by_id(f.document->document, "one"), *two = wa_by_id(f.document->document, "two");
    wa_css_add(7, one, "spin", 1000, 1000, 0, INFINITY);
    WaCssRecord *fade = wa_css_add(9, two, "fade", 900, 500, 0, 1);
    fade->r.paused = fade->r.style_paused = true;
    fade->r.hold_ms = 200;
    fade->r.fill = GARB_FILL_FORWARDS;
    check(os64_dom_web_animations_next(f.binding) == UINT64_MAX,
          "web animations: css: no CSSAnimation and no listener, nothing to step");
    p2b_script(&f,
        "throws('TypeError',()=>new CSSAnimation());"
        "const s=document.getElementById('one').animate(null,100);"
        "const all=document.getAnimations();"
        "assert(all.length===3&&all[2]===s,'css first: '+all.length);"
        "const [a,b]=all;globalThis.a=a;globalThis.b=b;"
        "assert(a instanceof CSSAnimation&&a instanceof Animation&&!(s instanceof CSSAnimation));"
        "assert(a.animationName==='spin'&&b.animationName==='fade');"
        "assert(Object.getPrototypeOf(CSSAnimation.prototype)===Animation.prototype);"
        "assert(document.getAnimations()[0]===a&&document.getElementById('two').getAnimations()[0]===b,'the same');"
        "assert(a.playState==='running'&&a.currentTime===0&&a.startTime===0,a.playState+' '+a.currentTime+' '+a.startTime);"
        "assert(b.playState==='paused'&&b.currentTime===200&&b.startTime===null);"
        "assert(a.effect instanceof KeyframeEffect&&a.effect.target===document.getElementById('one'));"
        "assert(a.effect.getTiming().duration===1000&&a.effect.getTiming().iterations===Infinity);"
        "assert(b.effect.getTiming().fill==='forwards'&&b.effect.getKeyframes().length===0);"
        "throws('TypeError',()=>Object.getOwnPropertyDescriptor(CSSAnimation.prototype,'animationName').get.call(s));",
        "web animations: css: CSSAnimations first, the same objects");
    check(wa_css_counted_current > 0, "web animations: css: getAnimations asks for the style made current");
    check(wa_css_records[0].scripted && !wa_css_records[0].r.paused && wa_css_records[0].r.start_ms == 1000 &&
              wa_css_records[1].scripted && wa_css_records[1].r.paused && wa_css_records[1].r.hold_ms == 200,
          "web animations: css: a CSSAnimation drives its host's clock, as it found it");
    /* A pause completes at the step; the host's clock holds there. */
    p2b_script(&f, "a.pause();assert(a.pending&&a.playState==='paused');", "web animations: css: pause");
    p2b_advance(100);
    wa_step(&f, "web animations: css: the pause's step");
    check(wa_css_records[0].r.paused && wa_css_records[0].r.hold_ms == 100,
          "web animations: css: the host holds where the pause did");
    p2b_script(&f, "assert(!a.pending&&a.currentTime===100&&a.playState==='paused',a.currentTime);",
               "web animations: css: paused at 100");
    /* animation-play-state turns running: a play, which does not rewind. */
    wa_css_records[1].r.style_paused = false;
    wa_css_version++;
    check(os64_dom_web_animations_next(f.binding) <= p2b_ms + 20,
          "web animations: css: the host's animations moved, a step is due");
    p2b_advance(20);
    wa_step(&f, "web animations: css: the play state's step");
    p2b_script(&f, "assert(b.pending&&b.currentTime===200,b.pending+' '+b.currentTime);",
               "web animations: css: running again, from where it was");
    p2b_advance(20);
    wa_step(&f, "web animations: css: the play's step");
    check(!wa_css_records[1].r.paused && wa_css_records[1].r.start_ms == p2b_ms - 200 &&
              wa_css_records[1].r.rate == 1,
          "web animations: css: the host runs it on from 200");
    /* The rate and a seek go to the host; a cancel cancels it there. */
    p2b_script(&f, "a.playbackRate=2;a.currentTime=300;", "web animations: css: rate and seek");
    check(wa_css_records[0].r.paused && wa_css_records[0].r.hold_ms == 300 && wa_css_records[0].r.rate == 2,
          "web animations: css: the host holds at the seek, at the rate");
    p2b_script(&f,
        "globalThis.log=[];a.finished.catch(e=>log.push(e.name));a.oncancel=()=>log.push('cancel');a.cancel();"
        "assert(a.playState==='idle'&&document.getAnimations().indexOf(a)<0);",
        "web animations: css: cancel");
    check(wa_css_records[0].r.canceled, "web animations: css: the host's animation canceled");
    p2b_advance(20);
    wa_step(&f, "web animations: css: the cancel's step");
    p2b_script(&f, "assert(log.join()==='AbortError,cancel',log.join());",
               "web animations: css: its promise rejected, its event fired");
    /* The style's new timing becomes its effect's. */
    wa_css_records[1].r.duration_ms = 800;
    wa_css_version++;
    p2b_advance(20);
    wa_step(&f, "web animations: css: the style's timing moved");
    p2b_script(&f, "assert(b.effect.getTiming().duration===800,b.effect.getTiming().duration);",
               "web animations: css: retimed");
    /* A CSSAnimation nothing holds, finished and no longer relevant, is
     * the same object when its style makes it relevant again. */
    WaCssRecord *done = wa_css_add(12, wa_by_id(f.document->document, "box"), "done", (double)p2b_ms - 500, 1000, 0, 1);
    p2b_script(&f, "document.getElementById('box').getAnimations()[0].tag='kept';",
               "web animations: css: an expando on an object nobody keeps");
    p2b_advance(600);
    wa_step(&f, "web animations: css: done finishes");
    os64_js_outcome_t gc;
    JS_RunGC(JS_GetRuntime(os64_js_context(f.runtime, OS64_JS_ABI_ID, &gc)));
    p2b_script(&f, "assert(document.getElementById('box').getAnimations().length===0);",
               "web animations: css: finished, not relevant");
    done->r.iterations = INFINITY;
    wa_css_version++;
    p2b_advance(20);
    wa_step(&f, "web animations: css: done runs again");
    p2b_script(&f, "assert(document.getElementById('box').getAnimations()[0].tag==='kept');",
               "web animations: css: the same object");
    wa_css_n--;
    wa_css_version++;
    p2b_advance(20);
    wa_step(&f, "web animations: css: done went");
    /* One the page laid out since the timeline's time was sampled waits
     * for the next step to start, as a play does. */
    wa_css_add(11, wa_by_id(f.document->document, "box"), "late", (double)p2b_ms + 5, 1000, 0, 1);
    p2b_script(&f,
        "const l=document.getElementById('box').getAnimations()[0];"
        "assert(l.animationName==='late'&&l.pending&&l.startTime===null&&l.currentTime===0,"
        "l.pending+' '+l.startTime+' '+l.currentTime);globalThis.late=l;",
        "web animations: css: a new one pending");
    p2b_advance(20);
    wa_step(&f, "web animations: css: the late one starts");
    p2b_script(&f, "assert(!late.pending&&late.startTime!==null&&late.currentTime===0,late.currentTime);",
               "web animations: css: started at the step");
    wa_css_n--;
    wa_css_version++;
    /* fade's CSS animation goes: its CSSAnimation is canceled and stays a
     * CSSAnimation, no longer the style's. */
    wa_css_records[1] = wa_css_records[--wa_css_n];
    wa_css_version++;
    p2b_script(&f, "b.oncancel=()=>log.push('b cancel');", "web animations: css: listen for fade's cancel");
    p2b_advance(20);
    wa_step(&f, "web animations: css: fade went");
    p2b_script(&f,
        "assert(b.playState==='idle'&&b.animationName==='fade',b.playState);"
        "assert(document.getElementById('two').getAnimations().length===0);"
        "assert(log.join()==='AbortError,cancel,b cancel',log.join());",
        "web animations: css: canceled, its name kept, its cancel heard");
    fixture_free(&f);
}

/* The CSS animation events (CSS Animations 2 § 4.2): each boundary an
 * animation crosses, heard where it bubbles, with its name and elapsed
 * time; the step is due at the next boundary and not before; a CSS
 * animation that goes before its end owes a cancel; AnimationEvent's
 * constructor and the handlers. */
static void wa_css_events(void)
{
    p2b_ms = 1000; p2b_us = 1000000;
    os64_dom_options_t options = p2b_options(P2B_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    wa_css_install(&f);
    const os64_html_node_t *box = wa_by_id(f.document->document, "box"), *one = wa_by_id(f.document->document, "one");
    p2b_script(&f,
        "for(const h of ['onanimationstart','onanimationend','onanimationiteration','onanimationcancel'])"
        "assert(h in window&&h in document&&h in document.body,h);"
        "const e=new AnimationEvent('animationend',{animationName:'x',elapsedTime:1.5});"
        "assert(e instanceof Event&&e.animationName==='x'&&e.elapsedTime===1.5&&e.pseudoElement===''&&!e.bubbles);"
        "const z=new AnimationEvent('animationstart');assert(z.animationName===''&&z.elapsedTime===0);"
        "globalThis.log=[];"
        "for(const t of ['animationstart','animationiteration','animationend','animationcancel'])"
        "document.addEventListener(t,ev=>log.push(ev.type+':'+ev.animationName+':'+ev.elapsedTime+':'+ev.target.id+':'"
        "+(ev instanceof AnimationEvent)+':'+ev.bubbles+':'+ev.cancelable));",
        "web animations: css events: the constructor, the handlers and a listener");
    check(os64_dom_web_animations_next(f.binding) == 0, "web animations: css events: a listener came: a look at once");
    wa_step(&f, "web animations: css events: the first look");
    check(os64_dom_web_animations_next(f.binding) == UINT64_MAX,
          "web animations: css events: no CSS animation, nothing due");
    wa_css_add(3, box, "pulse", 1000, 200, 100, 2);
    p2b_advance(20);
    wa_step(&f, "web animations: css events: pulse seen before its delay");
    check(os64_dom_web_animations_next(f.binding) == 1100,
          "web animations: css events: due when its active interval begins");
    p2b_advance(80);
    wa_step(&f, "web animations: css events: started");
    check(os64_dom_web_animations_next(f.binding) == 1300,
          "web animations: css events: due at the next iteration");
    p2b_advance(200);
    wa_step(&f, "web animations: css events: the second iteration");
    p2b_advance(200);
    wa_step(&f, "web animations: css events: ended");
    p2b_script(&f,
        "assert(log.join()==='animationstart:pulse:0:box:true:true:false,animationiteration:pulse:0.2:box:true:true:false,"
        "animationend:pulse:0.4:box:true:true:false',log.join());log.length=0;",
        "web animations: css events: start, iteration, end");
    /* Over in one step: start and end, in the order they happened beside
     * another's start; one that goes while active: a cancel at its active
     * time when it went, not when a step last looked. */
    wa_css_add(4, one, "blink", 1500, 50, 0, 1);
    WaCssRecord *gone = wa_css_add(5, one, "gone", 1500, 1000, 0, 1);
    (void)gone;
    p2b_advance(100);
    wa_step(&f, "web animations: css events: blink over, gone running");
    wa_css_n--;
    wa_css_version++;
    p2b_advance(150);
    wa_step(&f, "web animations: css events: gone went");
    p2b_script(&f,
        "assert(log.join()==='animationstart:blink:0:one:true:true:false,animationstart:gone:0:one:true:true:false,"
        "animationend:blink:0.05:one:true:true:false,animationcancel:gone:0.25:one:true:true:false',log.join());",
        "web animations: css events: together, and a cancel");
    /* Backwards across an iteration's start: the boundary crossed is the
     * later iteration's start, one second in. */
    p2b_script(&f, "log.length=0;", "web animations: css events: a new log");
    WaCssRecord *back = wa_css_add(6, box, "back", (double)p2b_ms + 1500, 1000, 0, 3);
    back->r.rate = -1;
    p2b_advance(20);
    wa_step(&f, "web animations: css events: back seen in its second iteration");
    p2b_advance(600);
    wa_step(&f, "web animations: css events: back into its first");
    p2b_script(&f, "assert(log.join()==='animationstart:back:0:box:true:true:false,"
               "animationiteration:back:1:box:true:true:false',log.join());",
               "web animations: css events: the boundary it crossed");
    /* Two that go together are canceled in composite order as they last
     * stood, after their list turned round. */
    p2b_script(&f, "log.length=0;", "web animations: css events: another log");
    wa_css_add(7, box, "c1", (double)p2b_ms, 1000, 0, 1);
    wa_css_add(8, box, "c2", (double)p2b_ms, 1000, 0, 1);
    p2b_advance(20);
    wa_step(&f, "web animations: css events: c1 and c2 start");
    /* The list turns round: c2, then c1. */
    WaCssRecord swap = wa_css_records[wa_css_n - 1];
    wa_css_records[wa_css_n - 1] = wa_css_records[wa_css_n - 2];
    wa_css_records[wa_css_n - 2] = swap;
    wa_css_version++;
    p2b_advance(20);
    wa_step(&f, "web animations: css events: c2 before c1");
    wa_css_n -= 2;
    wa_css_version++;
    p2b_advance(20);
    wa_step(&f, "web animations: css events: both go");
    p2b_script(&f, "assert(log.join()==='animationstart:c1:0:box:true:true:false,animationstart:c2:0:box:true:true:false,"
               "animationcancel:c2:0.06:box:true:true:false,animationcancel:c1:0.06:box:true:true:false',log.join());",
               "web animations: css events: canceled in composite order, as it last stood");
    fixture_free(&f);
}

/* A listener that comes after a CSS animation ended does not hear its end
 * (the `{once: true}` listener's next one), and still hears the start and
 * end of one that began since the step before it. */
static void wa_css_late_listener(void)
{
    p2b_ms = 1000; p2b_us = 1000000;
    os64_dom_options_t options = p2b_options(P2B_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    wa_css_install(&f);
    const os64_html_node_t *box = wa_by_id(f.document->document, "box");
    p2b_script(&f,
        "globalThis.log=[];globalThis.h=e=>log.push(e.type+':'+e.animationName);"
        "document.addEventListener('animationend',h,{once:true});",
        "web animations: css late: a listener once");
    wa_css_add(1, box, "first", 1000, 50, 0, 1);
    p2b_advance(20);
    wa_step(&f, "web animations: css late: first seen");
    p2b_advance(100);
    wa_step(&f, "web animations: css late: first ends, heard once");
    p2b_advance(20);
    wa_step(&f, "web animations: css late: nobody listens now");
    p2b_script(&f, "document.addEventListener('animationend',h);document.addEventListener('animationstart',h);",
               "web animations: css late: listeners again");
    wa_css_add(2, box, "fresh", (double)p2b_ms + 5, 50, 0, 1);
    p2b_advance(20);
    wa_step(&f, "web animations: css late: listening again");
    p2b_advance(100);
    wa_step(&f, "web animations: css late: fresh ends");
    p2b_script(&f, "assert(log.join()==='animationend:first,animationstart:fresh,animationend:fresh',log.join());",
               "web animations: css late: no stale end, the fresh one's heard");
    fixture_free(&f);
}

static void dom_webanimation_cases(void)
{
    wa_composite();
    wa_x_key();
    wa_running();
    wa_procedures();
    wa_collection();
    wa_frame_time();
    wa_effects();
    wa_replace();
    wa_css();
    wa_css_events();
    wa_css_late_listener();
}
