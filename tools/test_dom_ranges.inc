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
        "var dt=document.implementation.createDocument(null,null,document.implementation.createDocumentType('x','',''));"
        "var kept=document.body.appendChild(document.createElement('p'));var at=new Range();at.setStart(dt,0);"
        "assert(throwsName(()=>at.insertNode(kept),'HierarchyRequestError')&&kept.parentNode===document.body,"
        "'a refused node stays where it was');"
        "var m=made('<a></a><b></b><i></i>'),first=m.firstChild;var end=document.createRange();end.setStart(m,3);"
        "end.insertNode(first);assert(m.lastChild===first&&end.startContainer===m&&end.startOffset===2&&"
        "end.endOffset===3,'a node moved within its parent: the offset counts it gone from its old place');"
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
        "document.body.appendChild(f);"
        "var d=document.implementation.createHTMLDocument('');var dr=d.createRange();dr.selectNodeContents(d.body);"
        "var df=dr.createContextualFragment('<i>x</i>');assert(df.ownerDocument===d&&df.firstChild.ownerDocument===d,"
        "'a contextual fragment is its document\\'s');var dt=d.createElement('p');d.body.appendChild(dt);"
        "dt.textContent='ab';dr.selectNodeContents(dt);assert(dr.cloneContents().ownerDocument===d&&"
        "dr.extractContents().ownerDocument===d,'cloned and extracted contents are their document\\'s');"
        "var lone=d.createTextNode('ab');assert(lone.splitText(1).ownerDocument===d,'a parentless split tail too');"
        "var sp=document.body.appendChild(document.createElement('p'));sp.textContent='ab';var sr=new Range();"
        "sr.selectNodeContents(sp);var wrap=document.createElement('em');wrap.textContent='old';sr.surroundContents(wrap);"
        "assert(sp.innerHTML==='<em>ab</em>','surroundContents empties its new parent with the host hearing scripts');"
        "globalThis.built=[];customElements.define('x-range',class extends HTMLElement{constructor(){super();"
        "built.push(this);this.ready=true}});var cf=r.createContextualFragment('<x-range></x-range>');"
        "assert(cf.firstChild.ready===true&&built.length===1,'a defined custom element is upgraded at once');",
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

/* A fragment parse with no element of its own for context takes the
 * binding's one held body, not a new one each call: libhtml keeps a node
 * that was never inserted charged to its document until the document goes,
 * so a feed parsing pieces forever would fill the arena with bodies.
 * insertAdjacentHTML's fragment is reclaimed once inserted, so it leaves
 * nothing; createContextualFragment hands its fragment back, which stays
 * charged as any never-inserted node does (a dropped createDocumentFragment
 * too) — one node a call, never the body besides. */
static void dom_range_context_cases(void)
{
    os64_dom_options_t options = range_options();
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, RANGE_ASSERT "globalThis.cr=new Range();cr.createContextualFragment('<b>0</b>');"
               "document.documentElement.insertAdjacentHTML('afterbegin','');", "range-context-first");
    size_t before = f.document->node_count;
    dom_script(&f, RANGE_ASSERT "for(var i=0;i<100;i++)document.documentElement.insertAdjacentHTML('afterbegin','');",
               "range-context-adjacent");
    check(f.document->node_count == before, "ranges: insertAdjacentHTML's body-context parses make no node");
    dom_script(&f, RANGE_ASSERT "for(var i=0;i<100;i++)cr.createContextualFragment('');", "range-context-many");
    if (f.document->node_count != before + 100)
        fprintf(stderr, "nodes: %zu -> %zu\n", before, f.document->node_count);
    check(f.document->node_count == before + 100, "ranges: a contextual fragment leaves its fragment, no body");
    fixture_free(&f);
}

/* elementFromPoint and elementsFromPoint (libdom/hit.c) over a fake hit
 * provider: what it answers, retargeted against the root asked. */
static const os64_html_node_t *hit_answer[8];
static size_t hit_count, hit_listed, hit_calls;
static double hit_x, hit_y;

static size_t fake_hit(void *opaque, double x, double y, const os64_html_node_t **out, size_t cap,
                       os64_dom_geometry_stats_t *cost)
{
    (void)opaque;
    hit_calls++;
    hit_x = x;
    hit_y = y;
    cost->layouts = 1;
    /* hit_listed answers past the 8 held repeat the last: a deep list. */
    size_t listed = hit_listed > hit_count ? hit_listed : hit_count;
    for (size_t i = 0; i < listed && i < cap; i++) out[i] = hit_answer[i < hit_count ? i : hit_count - 1];
    return listed;
}

static void dom_hit_cases(void)
{
    os64_dom_options_t options = range_options();
    options.shadow_trees = true;
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, RANGE_ASSERT
        "assert(document.elementFromPoint(1,1)===null&&document.elementsFromPoint(1,1).length===0,'no provider');"
        "var host=document.createElement('div');host.id='host';document.body.appendChild(host);"
        "var root=host.attachShadow({mode:'open'});root.innerHTML='<p><b id=inner>x</b></p><slot></slot>';"
        "var light=document.createElement('i');light.id='light';host.appendChild(light);"
        "globalThis.root=root;",
        "hit-setup");
    const os64_html_node_t *host = native_id(f.document->document, "host");
    const os64_html_node_t *light = native_id(f.document->document, "light");
    const os64_html_node_t *inner = host != NULL && host->shadow_root != NULL
        ? native_id(host->shadow_root->first_child, "inner") : NULL;
    check(host != NULL && light != NULL && inner != NULL, "hit: the nodes are there");
    if (host == NULL || light == NULL || inner == NULL) { fixture_free(&f); return; }
    os64_dom_set_hit(f.binding, fake_hit, NULL);
    /* A hit inside the shadow tree: inner, its p, the host; then body, html. */
    hit_count = 0;
    hit_answer[hit_count++] = inner;
    hit_answer[hit_count++] = inner->parent;
    hit_answer[hit_count++] = host;
    hit_answer[hit_count++] = host->parent;
    hit_answer[hit_count++] = host->parent->parent;
    hit_calls = 0;
    dom_script(&f, RANGE_ASSERT
        "assert(document.elementFromPoint(12.5,7)===document.getElementById('host'),'retargeted to the host');"
        "var all=document.elementsFromPoint(3,4);"
        "assert(all.length===3&&all[0].id==='host'&&all[1]===document.body&&all[2]===document.documentElement,"
        "'the host once, then its ancestors');"
        "assert(root.elementFromPoint(1,1).id==='inner','a shadow root keeps its own');"
        "assert(root.elementsFromPoint(1,1).length===5,'and lists every element there');"
        "var bad=0;try{document.elementFromPoint(NaN,1)}catch(e){bad+=e instanceof TypeError}"
        "try{document.elementsFromPoint(1,Infinity)}catch(e){bad+=e instanceof TypeError}"
        "try{document.elementFromPoint(1)}catch(e){bad+=e instanceof TypeError}"
        "assert(bad===3,'two finite doubles');"
        "var other=document.implementation.createHTMLDocument('');"
        "assert(other.elementFromPoint(1,1)===null&&other.elementsFromPoint(1,1).length===0,'a document of its own has no viewport');",
        "hit-answers");
    check(hit_calls == 4 && hit_x == 1 && hit_y == 1, "hit: the provider is asked for the window's points only");
    hit_count = 1;
    hit_answer[0] = light;
    dom_script(&f, RANGE_ASSERT "assert(document.elementFromPoint(5,5).id==='light'&&root.elementFromPoint(5,5).id==='light',"
               "'a light-tree hit answers itself to both');", "hit-light");
    /* A list longer than the first ask's room is asked again, and both
     * asks' layouts are charged. */
    hit_listed = 70;
    os64_dom_geometry_stats(f.binding, true);
    hit_calls = 0;
    dom_script(&f, RANGE_ASSERT "assert(document.elementsFromPoint(5,5).length===1,'the repeats folded');", "hit-deep");
    os64_dom_geometry_stats_t stats = os64_dom_geometry_stats(f.binding, false);
    check(hit_calls == 2 && stats.layouts == 2, "hit: a re-ask charges both asks' layouts");
    hit_listed = 0;
    fixture_free(&f);
}

/* The page Selection as a model (range.c, RANGE.md § 9). */
static void dom_range_selection_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (fixture_ready(&f)) {
        dom_script(&f, RANGE_ASSERT "var s=getSelection();assert(s.rangeCount===0&&s.type==='None'&&!('addRange' in s),"
                   "'without ranges, the None selection');", "selection-off");
        fixture_free(&f);
    }
    os64_dom_options_t options = range_options();
    options.shadow_trees = true;
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, RANGE_ASSERT
        "var s=getSelection();assert(s===document.getSelection()&&s.rangeCount===0&&s.type==='None'&&s.isCollapsed,'empty');"
        "var p=document.createElement('p');p.innerHTML='ab<b>cd</b>ef';document.body.appendChild(p);"
        "var t=p.firstChild,e=p.lastChild,r=document.createRange();r.setStart(t,1);r.setEnd(e,1);"
        "s.addRange(r);assert(s.rangeCount===1&&s.getRangeAt(0)===r&&s.type==='Range'&&!s.isCollapsed,'addRange keeps it');"
        "assert(s.anchorNode===t&&s.anchorOffset===1&&s.focusNode===e&&s.focusOffset===1,'anchor and focus');"
        "assert(s.toString()==='bcde','toString');assert(s.containsNode(p.childNodes[1])&&!s.containsNode(p)&&"
        "s.containsNode(p,true),'containsNode');"
        "var other=document.createRange();s.addRange(other);assert(s.getRangeAt(0)===r,'a second addRange is ignored');"
        "var bad=false;try{s.getRangeAt(1)}catch(x){bad=x.name==='IndexSizeError'}assert(bad,'getRangeAt past the count');"
        "bad=false;try{s.removeRange(other)}catch(x){bad=x.name==='NotFoundError'}assert(bad,'removeRange of another');"
        "s.removeRange(r);assert(s.rangeCount===0,'removeRange');"
        "s.collapse(e,1);assert(s.type==='Caret'&&s.anchorNode===e&&s.getRangeAt(0).collapsed,'collapse');"
        "s.extend(t,0);assert(s.direction==='backward'&&s.anchorNode===e&&s.anchorOffset===1&&s.focusNode===t&&"
        "s.focusOffset===0&&s.toString()==='abcde','extend backward');"
        "s.collapseToStart();assert(s.anchorNode===t&&s.anchorOffset===0&&s.isCollapsed,'collapseToStart');"
        "s.setBaseAndExtent(e,2,t,1);assert(s.direction==='backward'&&s.toString()==='bcdef','setBaseAndExtent');"
        "s.selectAllChildren(p);assert(s.anchorNode===p&&s.focusOffset===3&&s.direction==='forward','selectAllChildren');"
        "s.setPosition(null);assert(s.rangeCount===0,'setPosition(null) empties it');"
        "bad=false;try{s.collapseToEnd()}catch(x){bad=x.name==='InvalidStateError'}assert(bad,'collapseToEnd of none');"
        "bad=false;try{s.collapse(t,9)}catch(x){bad=x.name==='IndexSizeError'}assert(bad,'an offset past the end');"
        "var d=document.implementation.createHTMLDocument('');s.collapse(d.body,0);assert(s.rangeCount===0,"
        "'a point in another document does nothing');"
        "s.selectAllChildren(p.childNodes[1]);s.deleteFromDocument();assert(p.innerHTML==='ab<b></b>ef','deleteFromDocument');"
        "s.empty();assert(s.rangeCount===0);"
        /* Each member checks its point in the standard's order: extend asks
         * whether the node is in the document before whether the selection
         * is empty, and an Attr is a node in no document, not a refusal. */
        "bad='';try{s.extend(document.doctype,0)}catch(x){bad=x.name}assert(bad==='InvalidStateError','extend of none');"
        "s.extend(d.body,0);assert(s.rangeCount===0,'extend to another document does nothing, even empty');"
        "var at=document.createAttribute('x');s.collapse(at,0);s.selectAllChildren(at);s.setBaseAndExtent(at,0,t,0);"
        "assert(s.rangeCount===0&&!s.containsNode(at),'an Attr is a node in no document');"
        "bad='';try{s.collapse(at,1)}catch(x){bad=x.name}assert(bad==='IndexSizeError','an Attr\\'s length is 0');"
        "s.collapse(t,0);bad='';try{s.extend(document.doctype,0)}catch(x){bad=x.name}"
        "assert(bad==='InvalidNodeTypeError','extend to a doctype');"
        "bad='';try{s.setBaseAndExtent(document.doctype,0,t,0)}catch(x){bad=x.name}"
        "assert(bad==='InvalidNodeTypeError','setBaseAndExtent from a doctype');"
        "bad='';try{s.setBaseAndExtent(t,9,d.body,0)}catch(x){bad=x.name}"
        "assert(bad==='IndexSizeError','setBaseAndExtent checks offsets before documents');",
        "selection-model");
    /* The page's code inside a member, and the page's hold on its range. */
    dom_script(&f, RANGE_ASSERT
        "var s=getSelection(),p=document.body.appendChild(document.createElement('p'));p.textContent='abc';"
        "s.collapse(p.firstChild,1);s.extend(p,{valueOf(){p.textContent='';return 0}});"
        "assert(s.anchorNode===p&&s.focusNode===p,'extend reads the selection after the offset\\'s valueOf');"
        "var bad='';try{s.extend(p,{valueOf(){s.removeAllRanges();return 0}})}catch(x){bad=x.name}"
        "assert(bad==='InvalidStateError','a valueOf that empties the selection empties it for extend');"
        "s.collapse(p,0);bad='';try{s.getRangeAt({valueOf(){s.removeAllRanges();return 0}})}catch(x){bad=x.name}"
        "assert(bad==='IndexSizeError','and for getRangeAt');"
        "s.collapse(p,0);s.collapse(undefined);assert(s.rangeCount===0,'collapse(undefined) is collapse(null)');"
        "s.collapse(p,0);s.setPosition(undefined);assert(s.rangeCount===0,'and setPosition(undefined)');"
        "var r=document.createRange();r.selectNodeContents(p);s.addRange(r);r.selectNodeContents(document.createElement('i'));"
        "assert(s.rangeCount===1&&s.anchorNode===null&&s.anchorOffset===0&&s.focusNode===null,"
        "'a range the page moved out of the document has no anchor or focus');s.removeAllRanges();"
        "var host=document.body.appendChild(document.createElement('div')),root=host.attachShadow({mode:'open'});"
        "root.innerHTML='<b>x</b>';s.selectAllChildren(root.firstChild);assert(s.rangeCount===0,"
        "'selectAllChildren of a node in a shadow tree does nothing');"
        "s.collapse(root.firstChild,0);assert(s.rangeCount===1,'collapse to one does select');s.removeAllRanges();"
        "globalThis.order=[];customElements.define('x-sel',class extends HTMLElement{disconnectedCallback(){order.push('gone')}});"
        "var q=document.body.appendChild(document.createElement('div'));q.innerHTML='<x-sel></x-sel>';"
        "s.selectAllChildren(q);s.deleteFromDocument();order.push('returned');"
        "assert(order.join()==='gone,returned','deleteFromDocument runs its reactions before it returns: '+order);",
        "selection-page-code");
    fixture_free(&f);
    /* A page that puts an accessor on Object.prototype before it ever
     * selects: the selection is held where no prototype reaches. */
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, RANGE_ASSERT
        "var seen=0;Object.defineProperty(Object.prototype,'r',{configurable:true,get(){seen++},set(v){seen++}});"
        "var s=getSelection(),p=document.body.appendChild(document.createElement('p'));"
        "s.collapse(p,0);var kept=s.rangeCount===1&&s.getRangeAt(0).startContainer===p;delete Object.prototype.r;"
        "assert(kept&&seen===0,'an accessor on Object.prototype neither sees nor answers for the selection');",
        "selection-prototype");
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
    dom_range_context_cases();
    dom_hit_cases();
    dom_range_selection_cases();
}
