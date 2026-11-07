/* RANGE.md § 5-§ 6: DOM Range's interface (libdom/range.c), behind
 * options.ranges. The live boundary points are libhtml's, held by its own
 * reference-model suite (tools/test_html_ranges.inc); these cases hold
 * what a page reaches, and WPT's dom/ranges examines the algorithms. */

/* libdom's internal.h: the harness links the binding's own objects. */
const char *d_ranges_name(size_t index);

#define RANGE_ASSERT "function assert(x,m){if(!x)throw Error(m||'assertion');}" \
    "function throwsName(f,n){try{f()}catch(e){return e.name===n||(n==='TypeError'&&e instanceof TypeError)}return false;}"

static os64_dom_options_t range_options(void)
{
    os64_dom_options_t options = os64_dom_default_options();
    options.ranges = true;
    return options;
}

static void dom_range_switch_cases(void)
{
    check(!os64_dom_default_options().ranges, "ranges: the switch is off unless a host turns it on");
    DomFixture f = fixture_new(NULL);
    if (fixture_ready(&f)) {
        dom_script(&f, RANGE_ASSERT
            "assert(typeof Range==='undefined'&&typeof AbstractRange==='undefined'&&typeof StaticRange==='undefined',"
            "'interfaces');assert(!('createRange' in document)&&typeof document.createRange==='undefined','createRange');",
            "range-off");
        fixture_free(&f);
    }
    os64_dom_options_t options = range_options();
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    for (size_t i = 0; d_ranges_name(i) != NULL; i++) {
        char script[256];
        snprintf(script, sizeof(script), RANGE_ASSERT "assert(typeof %s==='function');", d_ranges_name(i));
        dom_script(&f, script, d_ranges_name(i));
    }
    dom_script(&f, RANGE_ASSERT
        "assert(throwsName(function(){new AbstractRange()},'TypeError'),'AbstractRange has no constructor');"
        "assert(Object.getPrototypeOf(Range.prototype)===AbstractRange.prototype&&"
        "Object.getPrototypeOf(StaticRange.prototype)===AbstractRange.prototype,'both inherit AbstractRange');"
        "assert(Object.getPrototypeOf(Range)===AbstractRange,'the interface objects too');"
        "assert(Range.START_TO_START===0&&Range.START_TO_END===1&&Range.END_TO_END===2&&Range.END_TO_START===3"
        "&&Range.prototype.END_TO_START===3,'constants on the interface and its prototype');"
        "var r=new Range();assert(r instanceof Range&&r.startContainer===document&&r.startOffset===0&&r.collapsed,"
        "'new Range() is (document, 0)');var c=document.createRange();"
        "assert(c.endContainer===document&&c.endOffset===0&&Object.prototype.toString.call(c)==='[object Range]');"
        "class Mine extends Range{};assert(new Mine() instanceof Mine,'a subclass gets its prototype');",
        "range-interfaces");
    fixture_free(&f);
    options.modern = false;
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, RANGE_ASSERT "assert(typeof document.createRange==='undefined');", "range-modern-off");
    fixture_free(&f);
}

static void dom_range_placing_cases(void)
{
    os64_dom_options_t options = range_options();
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, RANGE_ASSERT
        "var p=document.createElement('p');p.innerHTML='ab<b>cd</b>ef';document.body.appendChild(p);"
        "var t=p.firstChild,b=p.childNodes[1],e=p.lastChild;var r=document.createRange();"
        "r.setStart(t,1);r.setEnd(e,1);assert(!r.collapsed&&r.commonAncestorContainer===p);"
        "assert(r.toString()==='bcde','toString cuts at both ends');"
        "assert(r.comparePoint(b,0)===0&&r.comparePoint(t,0)===-1&&r.comparePoint(e,2)===1,'comparePoint');"
        "assert(r.isPointInRange(b.firstChild,1)&&!r.isPointInRange(t,0),'isPointInRange');"
        "assert(r.intersectsNode(b)&&r.intersectsNode(t)&&!r.intersectsNode(document.head),'intersectsNode');"
        "r.setEnd(t,0);assert(r.collapsed&&r.startContainer===t&&r.startOffset===0,'an end before the start collapses');"
        "r.selectNode(b);assert(r.startContainer===p&&r.startOffset===1&&r.endOffset===2,'selectNode');"
        "r.selectNodeContents(b);assert(r.startContainer===b&&r.endOffset===1,'selectNodeContents');"
        "r.setStartBefore(b);r.setEndAfter(e);assert(r.startOffset===1&&r.endContainer===p&&r.endOffset===3);"
        "r.collapse(true);assert(r.collapsed&&r.endOffset===1,'collapse to the start');"
        "assert(throwsName(()=>r.setStart(t,9),'IndexSizeError')&&throwsName(()=>r.setStart(document.doctype,0),"
        "'InvalidNodeTypeError'),'set refuses');assert(throwsName(()=>r.setStartBefore(document),"
        "'InvalidNodeTypeError'),'no parent');assert(throwsName(()=>r.setStart({},0),'TypeError'),'not a node');"
        "var other=document.createElement('div');r.setStart(p,0);r.setEnd(p,2);r.setEnd(other,0);"
        "assert(r.startContainer===other,'another root collapses the range to the new point');"
        "var s=document.createRange();s.selectNodeContents(p);var q=s.cloneRange();"
        "assert(q!==s&&q.startContainer===p&&q.endOffset===3,'cloneRange');"
        "assert(s.compareBoundaryPoints(Range.START_TO_START,q)===0);q.setStart(b,0);"
        "assert(s.compareBoundaryPoints(Range.START_TO_START,q)===-1&&q.compareBoundaryPoints(Range.START_TO_START,s)===1);"
        "assert(throwsName(()=>s.compareBoundaryPoints(9,q),'NotSupportedError'),'how out of range');"
        "assert(throwsName(()=>s.compareBoundaryPoints(0,r),'WrongDocumentError'),'another tree');"
        "assert(s.detach()===undefined,'detach does nothing');",
        "range-placing");
    /* The live points follow the tree as the standard's steps move them. */
    dom_script(&f, RANGE_ASSERT
        "var d=document.createElement('div');d.innerHTML='<i>1</i><i>2</i><i>3</i>';document.body.appendChild(d);"
        "var r=document.createRange();r.setStart(d,1);r.setEnd(d.lastChild.firstChild,1);"
        "d.insertBefore(document.createElement('u'),d.firstChild);assert(r.startOffset===2,'an insertion before it');"
        "d.removeChild(d.childNodes[3]);assert(r.endContainer===d&&r.endOffset===3,'its end node removed');"
        "var t=new Text('hello');d.appendChild(t);r.setStart(t,2);r.setEnd(t,4);t.splitText(3);"
        "assert(r.startContainer===t&&r.endContainer===t.nextSibling&&r.endOffset===1,'splitText moves the end');"
        "t.deleteData(0,1);assert(r.startOffset===1,'deleteData shifts it');",
        "range-live");
    fixture_free(&f);
}

static void dom_range_content_cases(void)
{
    os64_dom_options_t options = range_options();
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, RANGE_ASSERT
        "function made(html){var d=document.createElement('div');d.innerHTML=html;document.body.appendChild(d);return d}"
        "var d=made('ab<b>cd</b>ef<i>gh</i>'),r=document.createRange();"
        "r.setStart(d.firstChild,1);r.setEnd(d.childNodes[3].firstChild,1);"
        "var c=r.cloneContents();assert(c.nodeType===11&&c.childNodes.length===4,'clone: four pieces');"
        "assert(c.firstChild.data==='b'&&c.childNodes[1].outerHTML==='<b>cd</b>'&&c.childNodes[2].data==='ef'"
        "&&c.lastChild.outerHTML==='<i>g</i>','clone: cut text, whole nodes, a partly held element');"
        "assert(d.innerHTML==='ab<b>cd</b>ef<i>gh</i>','clone leaves the tree');"
        "var x=r.extractContents();assert(x.childNodes.length===4&&d.innerHTML==='a<i>h</i>','extract moves it out');"
        "assert(r.collapsed&&r.startContainer===d&&r.startOffset===1,'and collapses between what is left');"
        "var e=made('one<b>two</b>three');r.setStart(e.firstChild,1);r.setEnd(e.lastChild,2);r.deleteContents();"
        "assert(e.innerHTML==='oree'&&r.collapsed&&r.startContainer===e&&r.startOffset===1,'deleteContents');"
        "var g=made('xy');r.setStart(g.firstChild,1);r.collapse(true);var n=document.createElement('hr');"
        "r.insertNode(n);assert(g.innerHTML==='x<hr>y'&&r.startContainer===g.firstChild&&r.startOffset===1&&"
        "r.endContainer===g&&r.endOffset===2,'insertNode splits the text, and a collapsed range takes the node in');"
        "var h=made('pq<s>r</s>t');r.setStart(h.firstChild,1);r.setEnd(h.lastChild,0);var w=document.createElement('em');"
        "r.surroundContents(w);assert(h.innerHTML==='p<em>q<s>r</s></em>t','surroundContents');"
        "assert(r.startContainer===h&&r.startOffset===1&&r.endOffset===2,'and selects the new parent');"
        "var k=made('a<b>bc</b>');r.setStart(k.firstChild,0);r.setEnd(k.lastChild.firstChild,1);"
        "assert(throwsName(()=>r.surroundContents(document.createElement('u')),'InvalidStateError'),"
        "'a partly held element refuses');"
        "r.selectNode(document.doctype);assert(throwsName(()=>r.cloneContents(),'HierarchyRequestError'),'a doctype');"
        "var cm=document.createComment('c');document.body.appendChild(cm);r.setStart(cm,0);"
        "assert(throwsName(()=>r.insertNode(document.createElement('a')),'HierarchyRequestError'),'into a comment');",
        "range-content");
    /* Each edit is an edit like any other: MutationObserver hears it. */
    dom_script(&f, RANGE_ASSERT
        "var d=document.createElement('div');d.innerHTML='one<b>two</b>three';document.body.appendChild(d);"
        "globalThis.records=[];new MutationObserver(function(r){records.push.apply(records,r)})"
        ".observe(d,{childList:true,characterData:true,subtree:true});"
        "var r=document.createRange();r.setStart(d.firstChild,1);r.setEnd(d.lastChild,2);r.deleteContents();",
        "range-records");
    dom_script(&f, RANGE_ASSERT
        "var kinds=records.map(function(r){return r.type}).join(',');"
        "assert(kinds==='characterData,childList,characterData','records: '+kinds);",
        "range-records-heard");
    fixture_free(&f);
}

static void dom_range_contextual_cases(void)
{
    os64_dom_options_t options = range_options();
    options.script_connected = event_connected;
    options.host_opaque = &event_clock;
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    connected_count = 0;
    dom_script(&f, RANGE_ASSERT
        "var r=document.createRange();r.selectNodeContents(document.body);"
        "var f=r.createContextualFragment('<p>a</p><td>dropped</td><script>void 0<\\/script>');"
        "assert(f.nodeType===11&&f.firstChild.outerHTML==='<p>a</p>','parsed as body content');"
        "assert(f.childNodes.length===3&&f.childNodes[1].nodeType===3,'a td outside a table is its text');"
        "var t=document.createElement('table');document.body.appendChild(t);r.selectNodeContents(t);"
        "var rows=r.createContextualFragment('<tr><td>1</td></tr>');assert(rows.firstChild.localName==='tbody'&&"
        "rows.firstChild.firstChild.localName==='tr','a table context keeps the row, in a tbody');"
        "var e=new Text('x');r.setStart(e,0);assert(r.createContextualFragment('<b>y</b>').firstChild.localName==='b',"
        "'a parentless text start parses in a body');"
        "document.body.appendChild(f);",
        "range-contextual");
    if (connected_count != 1) fprintf(stderr, "connected scripts: %u\n", connected_count);
    check(connected_count == 1, "ranges: a contextual fragment's script runs when it is inserted");
    fixture_free(&f);
}

static void dom_range_static_cases(void)
{
    os64_dom_options_t options = range_options();
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, RANGE_ASSERT
        "var t=new Text('abc');var s=new StaticRange({startContainer:t,startOffset:1,endContainer:t,endOffset:9});"
        "assert(s.startContainer===t&&s.startOffset===1&&s.endOffset===9&&!s.collapsed,'fields, unchecked offsets');"
        "assert(!('setStart' in s)&&!('commonAncestorContainer' in s),'none of Range\\'s members');"
        "t.data='';assert(s.endOffset===9,'never updated');"
        "assert(throwsName(()=>new StaticRange({startContainer:document.doctype,startOffset:0,endContainer:t,endOffset:0}),"
        "'InvalidNodeTypeError'),'a doctype');"
        "assert(throwsName(()=>new StaticRange({startContainer:t,startOffset:0,endContainer:t}),'TypeError'),'required');"
        "var a=document.createAttribute('x');assert(throwsName(()=>new StaticRange({startContainer:a,startOffset:0,"
        "endContainer:t,endOffset:0}),'InvalidNodeTypeError'),'an Attr');"
        "assert(throwsName(()=>Object.getOwnPropertyDescriptor(AbstractRange.prototype,'startOffset').get.call({}),"
        "'TypeError'),'a brand check');",
        "range-static");
    fixture_free(&f);
}

/* Dropped ranges are let go of at the next tree change, not the next
 * creation only: their records die with their objects and the sweep at
 * the top of the change frees them, releasing their holds. They are
 * dropped together, after the last creation, so no creation's sweep
 * reaches them first. */
static void dom_range_lifetime_cases(void)
{
    os64_dom_options_t options = range_options();
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, RANGE_ASSERT
        "globalThis.p=document.createElement('p');p.textContent='x';"
        "var all=[];for(var i=0;i<200;i++){var r=document.createRange();r.selectNodeContents(p.firstChild);all.push(r)}"
        "r=null;all=null;",
        "range-dropped");
    os64_js_outcome_t gc;
    JS_RunGC(JS_GetRuntime(os64_js_context(f.runtime, OS64_JS_ABI_ID, &gc)));
    size_t before = os64_dom_bytes(f.binding);
    dom_script(&f, RANGE_ASSERT "document.body.appendChild(document.createElement('hr'));", "range-sweep");
    check(os64_dom_bytes(f.binding) < before, "ranges: the next tree change frees the dropped ranges");
    fixture_free(&f);
}

static void dom_range_cases(void)
{
    dom_range_switch_cases();
    dom_range_placing_cases();
    dom_range_content_cases();
    dom_range_contextual_cases();
    dom_range_static_cases();
    dom_range_lifetime_cases();
}
