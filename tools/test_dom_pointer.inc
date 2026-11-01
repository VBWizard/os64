#define POINTER_ASSERT "function assert(x,m){if(!x)throw Error(m||'assertion');}"
#include <dom/pointer.h>

static void pointer_send(DomFixture *f, const char *id, os64_dom_pointer_action_t action,
                          int button, unsigned buttons)
{
    os64_dom_pointer_sample_t sample = {.action = action,
        .hit = id ? native_id(f->document->document, id) : NULL,
        .event = {.client_x = 10.0/3, .client_y = 20.0/3, .page_x = 16.5, .page_y = 20.5,
                  .page_coordinates = true, .button = button, .buttons = buttons}};
    os64_js_outcome_t out;
    os64_js_status_t status=os64_dom_pointer_dispatch(f->binding, &sample, NULL, NULL, NULL, &out);
    if (status!=OS64_JS_OK) fprintf(stderr,"pointer packet %d %s buttons=%u: status=%d %s\n",action,id?id:"null",buttons,status,out.message);
    check(status == OS64_JS_OK, "pointer: native packet completes");
}
static void pointer_capture_callbacks(void)
{
    for (int mode = 0; mode < 5; mode++) {
        DomFixture f = fixture_new(NULL);
        if (!fixture_ready(&f)) return;
        dom_script(&f, POINTER_ASSERT
            "document.body.innerHTML='<div id=a></div><div id=b></div><div id=c></div>';"
            "var a=document.getElementById('a'),b=document.getElementById('b'),c=document.getElementById('c'),log=[];"
            "for(let t of ['gotpointercapture','lostpointercapture','pointermove'])"
            "document.addEventListener(t,e=>log.push(t+':'+(e.target.id||'doc')));"
            "a.onpointerdown=()=>a.setPointerCapture(1);",
            "pointer: capture-listener fixture");
        const char *listeners[] = {
            "a.ongotpointercapture=()=>{a.releasePointerCapture(1);assert(!a.hasPointerCapture(1))};",
            "a.ongotpointercapture=()=>{b.setPointerCapture(1);assert(b.hasPointerCapture(1))};",
            "a.onlostpointercapture=()=>c.setPointerCapture(1);",
            "a.ongotpointercapture=()=>a.remove();",
            "a.onlostpointercapture=()=>b.remove();"
        };
        dom_script(&f, listeners[mode], "pointer: capture callback installed");
        pointer_send(&f, "a", OS64_DOM_POINTER_DOWN, 0, 1);
        pointer_send(&f, "b", OS64_DOM_POINTER_MOVE, -1, 1);
        if (mode == 2 || mode == 4) {
            dom_script(&f, "log=[];b.setPointerCapture(1);", "pointer: explicit pending transfer");
            pointer_send(&f, "c", OS64_DOM_POINTER_MOVE, -1, 1);
        }
        const char *first[] = {
            "assert(log.join()==='gotpointercapture:a,pointermove:a',log.join());",
            "assert(log.join()==='gotpointercapture:a,pointermove:a',log.join());",
            "assert(log.join()==='lostpointercapture:a,gotpointercapture:b,pointermove:b',log.join());",
            "assert(log.join()==='gotpointercapture:a,pointermove:doc',log.join());",
            "assert(log.join()==='lostpointercapture:a,pointermove:c',log.join());"
        };
        dom_script(&f, first[mode], "pointer: in-flight capture transition survives callback mutation");
        dom_script(&f, "log=[];", "pointer: clear transition log");
        pointer_send(&f, "c", OS64_DOM_POINTER_MOVE, -1, 1);
        const char *next[] = {
            "assert(log.join()==='lostpointercapture:a,pointermove:c',log.join());",
            "assert(log.join()==='lostpointercapture:a,gotpointercapture:b,pointermove:b',log.join());",
            "assert(log.join()==='lostpointercapture:b,gotpointercapture:c,pointermove:c',log.join());",
            "assert(log.join()==='lostpointercapture:doc,pointermove:c',log.join());",
            "assert(log.join()==='pointermove:c',log.join());"
        };
        dom_script(&f, next[mode], "pointer: callback request is processed on the next packet");
        pointer_send(&f, "c", OS64_DOM_POINTER_UP, 0, 0);
        fixture_free(&f);
    }
}
static void dom_pointer_cases(void)
{
    pointer_capture_callbacks();
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, POINTER_ASSERT
        "var pe=new PointerEvent('x',{clientX:1.25,buttons:1,pointerId:7,pointerType:'pen',pressure:0.5});"
        "assert(pe instanceof PointerEvent&&pe instanceof MouseEvent&&pe instanceof UIEvent&&pe instanceof Event);"
        "assert(Object.getPrototypeOf(PointerEvent)===MouseEvent);"
        "assert(pe.clientX===1.25&&pe.pointerId===7&&pe.pointerType==='pen'&&pe.pressure===0.5);"
        "assert(pe.width===1&&pe.height===1&&!pe.isPrimary&&pe.tiltX===0&&pe.twist===0);"
        "document.body.innerHTML='<div id=outer><div id=a></div><div id=b></div></div>';"
        "globalThis.a=document.getElementById('a');globalThis.b=document.getElementById('b');"
        "globalThis.log=[];"
        "for(let n of ['pointerdown','pointermove','pointerup','pointercancel','gotpointercapture','lostpointercapture',"
        "'mousedown','mousemove','mouseup','click','auxclick'])document.addEventListener(n,e=>{"
        "log.push(n+':'+(e.target.id||'doc'));if(n==='pointerdown')assert(e.clientX===10/3&&e.pageX===16.5)});"
        "var bad=false;try{a.setPointerCapture(1)}catch(e){bad=e.name==='NotFoundError'}assert(bad);"
        "a.onpointerdown=e=>{a.setPointerCapture(e.pointerId);assert(a.hasPointerCapture(1));e.preventDefault()};",
        "pointer: constructors and capture setup");
    pointer_send(&f,"a",OS64_DOM_POINTER_DOWN,0,1);
    pointer_send(&f,"b",OS64_DOM_POINTER_MOVE,-1,1);
    pointer_send(&f,"b",OS64_DOM_POINTER_UP,0,0);
    dom_script(&f, POINTER_ASSERT
        "assert(log.join()==='pointerdown:a,gotpointercapture:a,pointermove:a,pointerup:a,lostpointercapture:a,click:a',log.join());"
        "assert(!a.hasPointerCapture(1));log=[];a.onpointerdown=null;",
        "pointer: capture, suppression and captured click ordering");
    pointer_send(&f,"a",OS64_DOM_POINTER_DOWN,0,1);
    pointer_send(&f,"b",OS64_DOM_POINTER_UP,0,0);
    dom_script(&f, POINTER_ASSERT
        "assert(log.join()==='pointerdown:a,mousedown:a,pointerup:b,mouseup:b,click:outer',log.join());"
        "log=[];a.onpointerdown=e=>a.setPointerCapture(e.pointerId);",
        "pointer: uncaptured common-ancestor activation");
    pointer_send(&f,"a",OS64_DOM_POINTER_DOWN,0,1);
    pointer_send(&f,"b",OS64_DOM_POINTER_MOVE,-1,1);
    os64_dom_pointer_cancel(f.binding);
    pointer_send(&f,"b",OS64_DOM_POINTER_MOVE,-1,1);
    pointer_send(&f,"b",OS64_DOM_POINTER_UP,0,0);
    dom_script(&f, POINTER_ASSERT
        "assert(log.includes('pointercancel:a')&&log.includes('lostpointercapture:a'));"
        "assert(!log.some(x=>x.startsWith('click:')||x.startsWith('pointerup:')),log.join());"
        "log=[];a.onpointerdown=null;",
        "pointer: loss cancels and consumes the physical tail");
    pointer_send(&f,"a",OS64_DOM_POINTER_DOWN,0,1);
    pointer_send(&f,"a",OS64_DOM_POINTER_DOWN,1,5);
    pointer_send(&f,"a",OS64_DOM_POINTER_UP,1,1);
    pointer_send(&f,"a",OS64_DOM_POINTER_UP,0,0);
    dom_script(&f, POINTER_ASSERT
        "assert(log.filter(x=>x==='pointerdown:a').length===1&&log.filter(x=>x==='pointerup:a').length===1,log.join());"
        "assert(log.includes('auxclick:a')&&log.includes('click:a'),log.join());",
        "pointer: chord transitions keep one pointer stream");
    dom_script(&f, POINTER_ASSERT
        "log=[];a.onpointerdown=e=>a.setPointerCapture(1);"
        "a.onpointermove=e=>{assert(e instanceof PointerEvent&&e.pointerType==='mouse'&&e.isPrimary);"
        "assert(e.pressure===0.5);b.setPointerCapture(1)};",
        "pointer: transfer setup");
    pointer_send(&f,"a",OS64_DOM_POINTER_DOWN,0,1);
    pointer_send(&f,"a",OS64_DOM_POINTER_MOVE,-1,1);
    pointer_send(&f,"a",OS64_DOM_POINTER_MOVE,-1,1);
    dom_script(&f, POINTER_ASSERT
        "assert(log.filter(x=>x.indexOf('pointer')>=0).slice(-3).join()==='lostpointercapture:a,gotpointercapture:b,pointermove:b');"
        "document.addEventListener('lostpointercapture',e=>{if(e.target===document)log.push('fallback')});a.onpointermove=null;b.remove();",
        "pointer: capture transfer and removal");
    pointer_send(&f,"a",OS64_DOM_POINTER_MOVE,-1,1);
    pointer_send(&f,"a",OS64_DOM_POINTER_UP,0,0);
    dom_script(&f, POINTER_ASSERT
        "assert(log.includes('fallback'));a.onpointerdown=null;a.onpointermove=null;log=[];"
        "a.setPointerCapture(1);assert(!a.hasPointerCapture(1));"
        "var bad=false;try{b.setPointerCapture(1)}catch(e){bad=e.name==='InvalidStateError'}assert(bad);"
        "a.dispatchEvent(new PointerEvent('pointerdown',{pointerId:9,buttons:1}));"
        "bad=false;try{a.setPointerCapture(9)}catch(e){bad=e.name==='NotFoundError'}assert(bad);"
        "log=[];a.onpointerdown=e=>a.setPointerCapture(1);",
        "pointer: detached/hover errors and synthetic input isolation");
    pointer_send(&f,"a",OS64_DOM_POINTER_DOWN,0,1);
    for (int n=0;n<32;n++) pointer_send(&f,"outer",OS64_DOM_POINTER_MOVE,-1,1);
    pointer_send(&f,"outer",OS64_DOM_POINTER_UP,0,0);
    dom_script(&f, POINTER_ASSERT
        "assert(log.filter(x=>x==='pointermove:a').length===32);"
        "assert(log.filter(x=>x==='click:a').length===1);"
        "let p=new PointerEvent('x',{tiltY:45});assert(p.azimuthAngle===Math.PI/2&&p.altitudeAngle===Math.PI/4);"
        "p=new PointerEvent('x',{altitudeAngle:Math.PI/4});assert(p.tiltX===45&&p.tiltY===0);"
        "p=new PointerEvent('x',{tiltX:45,azimuthAngle:1});assert(p.altitudeAngle===Math.PI/2&&p.azimuthAngle===1);"
        "p=new PointerEvent('x',{twist:-1});assert(p.twist===-1);",
        "pointer: complete drag batch and orientation dictionaries");
    fixture_free(&f);
    f=fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, POINTER_ASSERT
        "document.body.innerHTML='';var moves=0,ups=0,cancels=0,enters=0;"
        "for(let id of ['a','b']){let parent=document.body;for(let n=0;n<20;n++){"
        "let child=document.createElement('div');parent.appendChild(child);parent=child}parent.id=id}"
        "document.onpointermove=()=>moves++;document.onpointerup=()=>ups++;"
        "document.onpointercancel=()=>cancels++;"
        "document.addEventListener('pointerenter',()=>enters++,true);",
        "pointer: deep boundary batch setup");
    pointer_send(&f,"a",OS64_DOM_POINTER_DOWN,0,1);
    for (int n=0;n<32;n++) pointer_send(&f,n%2?"a":"b",OS64_DOM_POINTER_MOVE,-1,1);
    pointer_send(&f,"a",OS64_DOM_POINTER_UP,0,0);
    dom_script(&f, POINTER_ASSERT
        "assert(moves===32&&ups===1&&cancels===0&&enters>600,[moves,ups,cancels,enters].join());",
        "pointer: deep boundary expansion cannot overflow a full native batch");
    fixture_free(&f);
    // Refuse early down and capture-transition allocations. Teardown must
    // release persistent capture, transition snapshots and ancestor paths.
    for (int transition=0;transition<2;transition++) for (size_t cut=1;cut<=24;cut++) {
        DomFixture failure=fixture_new(NULL);
        if (!fixture_ready(&failure)) { fixture_free(&failure); break; }
        dom_script(&failure,"document.body.innerHTML='<div id=a></div>';"
            "document.onpointerdown=e=>e.target.setPointerCapture(e.pointerId)","pointer: refusal setup");
        os64_dom_pointer_sample_t sample={.action=OS64_DOM_POINTER_DOWN,
            .hit=native_id(failure.document->document,"a"),.event={.button=0,.buttons=1}};
        os64_js_outcome_t out;
        if (transition) {
            pointer_send(&failure,"a",OS64_DOM_POINTER_DOWN,0,1);
            sample.action=OS64_DOM_POINTER_MOVE;sample.event.button=-1;
        }
        dom_fail_at=dom_attempts+cut;
        (void)os64_dom_pointer_dispatch(failure.binding,&sample,NULL,NULL,NULL,&out);
        dom_fail_at=0;
        fixture_free(&failure);
    }

}

#undef POINTER_ASSERT
