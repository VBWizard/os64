/* screen and devicePixelRatio (libdom/screen.c), performance
 * (libdom/performance.c) and URL and URLSearchParams (libdom/url.c): a
 * case per sentence of each file's contract, and the binding's allocations
 * failed in turn. */

static uint32_t view_zoom = 1000;
static uint32_t view_zoom_of(void *opaque) { (void)opaque; return view_zoom; }

static void dom_screen_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, STORAGE_ASSERT
        "assert(screen.width===1024&&screen.height===768&&screen.availWidth===1024&&screen.availHeight===768,"
        "'a host that says nothing is told the default');"
        "assert(screen.colorDepth===24&&screen.pixelDepth===24&&devicePixelRatio===1);"
        "assert(screen===window.screen&&screen instanceof Screen&&Object.prototype.toString.call(screen)==='[object Screen]');"
        "var o=screen.orientation;assert(o===screen.orientation&&o instanceof ScreenOrientation);"
        "assert(o.type==='landscape-primary'&&o.angle===0&&o.onchange===null&&o.unlock()===undefined);"
        "throws('TypeError',()=>new Screen());throws('TypeError',()=>new ScreenOrientation());"
        "throws('TypeError',()=>Object.getOwnPropertyDescriptor(Screen.prototype,'width').get.call({}));"
        "globalThis.locked=[];for(const a of [[],['sideways'],['portrait']])o.lock(...a).then(()=>locked.push('kept'),e=>locked.push(e.name));"
        /* An EventTarget: its listeners and its onchange run on a dispatch,
         * though nothing here ever changes the orientation. */
        "var heard=[];o.addEventListener('change',e=>heard.push('listener:'+(e.target===o)));"
        "o.onchange=e=>heard.push('handler');o.dispatchEvent(new Event('change'));"
        "assert(heard.join()==='listener:true,handler',heard.join());"
        "o.onchange=null;assert(o.onchange===null);",
        "screen: the default display, Screen and ScreenOrientation");
    dom_script(&f, STORAGE_ASSERT "assert(locked.join()==='TypeError,TypeError,NotSupportedError',locked.join());",
               "screen: lock() rejects: its argument's errors as TypeError, any lock as NotSupportedError");
    dom_script(&f, STORAGE_ASSERT
        "screen=5;assert(screen===5);devicePixelRatio=3;assert(devicePixelRatio===3);"
        "assert(Object.getOwnPropertyDescriptor(window,'screen').value===5,'[Replaceable]: an own data property');",
        "screen: screen and devicePixelRatio are [Replaceable]");
    fixture_free(&f);
    os64_dom_options_t options = os64_dom_default_options();
    options.screen = (os64_dom_screen_t){1920, 1080, 1920, 1040, 30};
    options.zoom = view_zoom_of;
    view_zoom = 1250;
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, STORAGE_ASSERT
        "assert(devicePixelRatio===1.25&&screen.width===1536&&screen.height===864&&screen.availHeight===832,"
        "'CSS pixels at the zoom: '+[devicePixelRatio,screen.width,screen.height,screen.availHeight]);"
        "assert(screen.colorDepth===30&&screen.pixelDepth===30);",
        "screen: the host's display, in CSS pixels at the zoom");
    view_zoom = 500;
    dom_script(&f, STORAGE_ASSERT "assert(devicePixelRatio===0.5&&screen.width===3840);",
               "screen: the zoom is read when asked");
    view_zoom = 1000;
    fixture_free(&f);
    options.screen = (os64_dom_screen_t){768, 1024, 768, 1024, 24};
    options.zoom = NULL;
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, STORAGE_ASSERT "assert(screen.orientation.type==='portrait-primary'&&devicePixelRatio===1);",
               "screen: a display taller than wide is portrait");
    fixture_free(&f);
    static const char *const kEngine[] = {
        "screen.orientation.lock('any').catch(()=>{});screen.width+screen.orientation.type",
        "var o=screen.orientation;o.onchange=()=>1;o.addEventListener('change',()=>2);o.dispatchEvent(new Event('change'))",
    };
    engine_sweep("screen", kEngine, sizeof(kEngine) / sizeof(kEngine[0]), NULL);
}

/* The host's timing: an origin and milestones a case sets. */
static os64_dom_timing_t view_timing;
static uint64_t view_us;
static void view_timing_of(void *opaque, os64_dom_timing_t *out) { (void)opaque; *out = view_timing; }
static uint64_t view_now_us(void *opaque) { (void)opaque; return view_us; }

static void dom_performance_cases(void)
{
    view_timing = (os64_dom_timing_t){.origin_us = 1000000, .origin_epoch_us = 1700000000123456};
    for (int i = 0; i < OS64_DOM_TIMING_COUNT; i++) view_timing.at[i] = OS64_DOM_TIMING_NOT_YET;
    view_timing.at[OS64_DOM_TIMING_FETCH_START] = 0;
    view_timing.at[OS64_DOM_TIMING_RESPONSE_START] = 2000;
    view_timing.at[OS64_DOM_TIMING_DOM_LOADING] = 2500;
    view_timing.type = OS64_DOM_NAVIGATE_RELOADED;
    view_timing.redirects = 2;
    view_us = 1000000 + 12345678;        /* 12345.678 ms after the origin */
    os64_dom_options_t options = os64_dom_default_options();
    options.url = "https://example.test/page?q";
    options.timing = view_timing_of;
    options.now_us = view_now_us;
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, STORAGE_ASSERT
        "assert(performance===window.performance&&performance instanceof Performance);"
        "assert(Object.prototype.toString.call(performance)==='[object Performance]');"
        "assert(performance.now()===12345.6,'coarsened to 100 microseconds: '+performance.now());"
        "assert(performance.timeOrigin===1700000000123.4,''+performance.timeOrigin);"
        "var t=performance.timing;assert(t===performance.timing&&t instanceof PerformanceTiming);"
        "assert(t.navigationStart===1700000000123&&t.fetchStart===1700000000123&&t.requestStart===t.fetchStart);"
        "assert(t.responseStart===1700000000125&&t.domLoading===1700000000125&&t.domInteractive===0);"
        "assert(t.secureConnectionStart===t.fetchStart&&t.unloadEventStart===0&&t.redirectEnd===0);"
        "assert(t.loadEventEnd===0&&t.toJSON().responseStart===t.responseStart&&Object.keys(t.toJSON()).length===21);"
        "var n=performance.navigation;assert(n.type===1&&n.redirectCount===2&&n.TYPE_RELOAD===1&&PerformanceNavigation.TYPE_BACK_FORWARD===2);"
        "var nav=performance.getEntriesByType('navigation');assert(nav.length===1);nav=nav[0];"
        "assert(nav.name==='https://example.test/page?q'&&nav.entryType==='navigation'&&nav.startTime===0);"
        "assert(nav.duration===0&&nav.responseStart===2&&nav.domInteractive===0&&nav.type==='reload'&&nav.redirectCount===2);"
        "assert(nav.initiatorType==='navigation'&&nav.nextHopProtocol==='http/1.1'&&Array.isArray(nav.serverTiming));"
        "assert(nav instanceof PerformanceEntry&&!('PerformanceNavigationTiming' in window)&&!('PerformanceResourceTiming' in window));"
        "assert(Object.prototype.toString.call(nav)==='[object PerformanceNavigationTiming]');"
        "var j=nav.toJSON();assert(j.name===nav.name&&j.domLoading===undefined&&j.type==='reload'&&j.responseStart===2);"
        "var p=performance.toJSON();assert(p.timeOrigin===performance.timeOrigin&&p.timing.domLoading===t.domLoading&&p.navigation.type===1);"
        "assert(performance.getEntriesByType('resource').length===0&&performance.getEntriesByType('paint').length===0);"
        "throws('TypeError',()=>new Performance());throws('TypeError',()=>performance.now.call({}));"
        "throws('TypeError',()=>Object.getOwnPropertyDescriptor(PerformanceTiming.prototype,'fetchStart').get.call(nav));",
        "performance: the clock, timing, navigation and the navigation entry");
    /* Milestones the host reaches later are read when asked. */
    view_timing.at[OS64_DOM_TIMING_DOM_INTERACTIVE] = 3000;
    view_timing.at[OS64_DOM_TIMING_LOAD_END] = 9000;
    dom_script(&f, STORAGE_ASSERT
        "assert(performance.timing.domInteractive===1700000000126&&performance.timing.loadEventEnd===1700000000132);"
        "var nav=performance.getEntriesByType('navigation')[0];assert(nav.duration===9&&nav.domInteractive===3);",
        "performance: a later milestone is read when asked");
    view_us += 1000;
    dom_script(&f, STORAGE_ASSERT
        "var m=performance.mark('m1');assert(m instanceof PerformanceMark&&m.entryType==='mark'&&m.name==='m1');"
        "assert(m.startTime===12346.6&&m.duration===0&&m.detail===null);"
        "var m2=performance.mark('m2',{startTime:5,detail:{a:[1,2]}});assert(m2.startTime===5&&m2.detail.a[1]===2);"
        "var d={x:1};var m3=performance.mark('m3',{detail:d});d.x=2;assert(m3.detail.x===1&&m3.detail!==d,'detail is cloned');"
        "assert(m3.detail===m3.detail,'the same clone each read');"
        "throws('DataCloneError',()=>performance.mark('bad',{detail:{s:Symbol()}}));"
        "throws('DataCloneError',()=>performance.mark('bad',{detail:()=>1}));"
        "throws('TypeError',()=>performance.mark('neg',{startTime:-1}));"
        "throws('TypeError',()=>performance.mark('nan',{startTime:NaN}));"
        "throws('TypeError',()=>performance.mark('n',5));throws('TypeError',()=>performance.mark());"
        "throws('SyntaxError',()=>performance.mark('navigationStart'));throws('SyntaxError',()=>new PerformanceMark('domComplete'));"
        "var e=performance.getEntries();assert(e.length===4&&e[0].entryType==='navigation'&&e[1]===m2&&e[2]===m&&e[3]===m3,"
        "'in startTime order');"
        "assert(performance.getEntriesByName('m1')[0]===m&&performance.getEntriesByName('m1','measure').length===0);"
        "var c=new PerformanceMark('c',{startTime:1});assert(c.startTime===1&&performance.getEntriesByName('c').length===0);"
        "class Sub extends PerformanceMark{};assert(new Sub('s') instanceof Sub);"
        "throws('TypeError',()=>PerformanceMark('x'));throws('TypeError',()=>new PerformanceEntry());"
        "assert(JSON.stringify(m.toJSON())==='{\"name\":\"m1\",\"entryType\":\"mark\",\"startTime\":12346.6,\"duration\":0}');",
        "performance: mark and PerformanceMark");
    dom_script(&f, STORAGE_ASSERT
        "var a=performance.measure('all');assert(a.startTime===0&&a.duration===12346.6&&a instanceof PerformanceMeasure);"
        "var b=performance.measure('between','m2','m1');assert(b.startTime===5&&Math.abs(b.duration-12341.6)<1e-9);"
        "assert(performance.measure('to','fetchStart','m2').startTime===0);"
        "var c=performance.measure('c',{start:'m2',duration:10});assert(c.startTime===5&&c.duration===10);"
        "var d=performance.measure('d',{duration:4,end:20,detail:'x'});assert(d.startTime===16&&d.detail==='x');"
        "var e=performance.measure('e',{start:7});assert(e.startTime===7);"
        "var g=performance.measure('g',undefined,'m2');assert(g.startTime===0&&g.duration===5);"
        "var h=performance.measure('h',null,'m2');assert(h.startTime===0);"
        "throws('TypeError',()=>performance.measure('x',{start:1},'m1'));"
        "throws('TypeError',()=>performance.measure('x',{duration:1}));"
        "throws('TypeError',()=>performance.measure('x',{start:1,end:2,duration:1}));"
        "throws('TypeError',()=>performance.measure('x',{start:-1}));"
        "throws('SyntaxError',()=>performance.measure('x','no such mark'));"
        "throws('InvalidAccessError',()=>performance.measure('x','loadEventStart'));"
        "throws('DataCloneError',()=>performance.measure('x',{start:0,detail:Symbol()}));"
        "assert(performance.getEntriesByType('measure').length===8);"
        "performance.clearMarks('m1');assert(performance.getEntriesByName('m1').length===0&&performance.getEntriesByType('mark').length===2);"
        "performance.clearMarks();assert(performance.getEntriesByType('mark').length===0&&performance.getEntriesByType('measure').length===8);"
        "performance.clearMeasures('all');assert(performance.getEntriesByType('measure').length===7);"
        "performance.clearMeasures();assert(performance.getEntries().length===1,'the navigation entry stays');"
        "throws('SyntaxError',()=>performance.measure('x','m1'));",
        "performance: measure, the queries and clearing");
    dom_script(&f, STORAGE_ASSERT
        "var heard=0;performance.addEventListener('x',()=>heard++,{once:true});"
        "performance.dispatchEvent(new Event('x'));performance.dispatchEvent(new Event('x'));assert(heard===1);"
        "performance=7;assert(performance===7,'[Replaceable]');",
        "performance: an EventTarget, and [Replaceable]");
    fixture_free(&f);
    /* With no timing from the host: the origin is the binding's making and
     * nothing has happened. */
    options.timing = NULL;
    options.url = "http://plain.test/";
    view_us = 5000000;
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    view_us += 2500;
    dom_script(&f, STORAGE_ASSERT
        "assert(performance.now()===2.5&&performance.timing.fetchStart===0&&performance.timing.secureConnectionStart===0);"
        "assert(performance.navigation.type===0&&performance.getEntriesByType('navigation')[0].type==='navigate');",
        "performance: a host with no timing");
    fixture_free(&f);
    static const char *const kOperations[] = {
        "performance.mark('m')", "performance.mark('m',{detail:{a:1}})", "performance.measure('x')",
        "performance.mark('a');performance.getEntries()", "performance.getEntriesByName('x','mark')",
        "performance.clearMarks('a')", "performance.toJSON()",
    };
    binding_sweep("performance", kOperations, sizeof(kOperations) / sizeof(kOperations[0]), &options);
    static const char *const kEngine[] = {
        "performance.mark('a',{detail:{x:[1,2]}});performance.measure('b','a');performance.getEntries().length",
        "performance.getEntriesByType('navigation')[0].toJSON()",
        "JSON.stringify(performance.toJSON())",
        "new PerformanceMark('n').toJSON()",
    };
    engine_sweep("performance", kEngine, sizeof(kEngine) / sizeof(kEngine[0]), &options);
}

static void dom_url_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, STORAGE_ASSERT
        "var u=new URL('HTTPS://Example.COM:443/a/../b?x=1&y=%41#frag');"
        "assert(u.href==='https://example.com/b?x=1&y=%41#frag',u.href);"
        "assert(u.protocol==='https:'&&u.host==='example.com'&&u.hostname==='example.com'&&u.port===''&&u.origin==='https://example.com');"
        "assert(u.pathname==='/b'&&u.search==='?x=1&y=%41'&&u.hash==='#frag'&&u.username===''&&u.password==='');"
        "assert(String(u)===u.href&&u.toString()===u.href&&u.toJSON()===u.href&&JSON.stringify({u})==='{\"u\":\"'+u.href+'\"}');"
        "assert(u instanceof URL&&Object.prototype.toString.call(u)==='[object URL]'&&URL.length===1);"
        "assert(new URL('../c?d#e','http://h.test:8080/a/b/f').href==='http://h.test:8080/a/c?d#e');"
        "assert(new URL('//cdn.test/x','https://h.test/').href==='https://cdn.test/x');"
        "assert(new URL('?q','http://h.test/p?old#f').href==='http://h.test/p?q');"
        "assert(new URL('#n','http://h.test/p?old#f').href==='http://h.test/p?old#n');"
        "assert(new URL('','http://h.test/p?old#f').href==='http://h.test/p?old');"
        "assert(new URL('http://h.test/a b?c d').href==='http://h.test/a%20b?c%20d','a space is escaped');"
        "assert(new URL('  http://h.test/\\t\\nx  ').href==='http://h.test/x','surrounding spaces and tab and newline go');"
        "assert(new URL('http://h.test/\\u00e9').pathname==='/%C3%A9');"
        "assert(new URL('\\u0000http://h.test/\\u0000x').href==='http://h.test/%00x','a NUL: trimmed at the ends, %00 within');"
        "assert(new URL('mailto:someone@example.test').href==='mailto:someone@example.test');"
        "var m=new URL('MAILTO:me');assert(m.protocol==='mailto:'&&m.pathname==='me'&&m.host===''&&m.origin==='null');"
        "assert(new URL('http://h.test','http://other.test/').href==='http://h.test/');"
        "assert(new URL('blob:https://h.test/uuid').origin==='https://h.test'&&new URL('blob:foo').origin==='null');"
        "assert(new URL('file:///tmp/x').origin==='null'&&new URL('ftp://h.test:21/').href==='ftp://h.test/');"
        "assert(new URL('ws://h.test:81/').origin==='ws://h.test:81');"
        "for(const bad of ['','relative','//h.test/x','http://','http://user@h.test/','http://[::1]/','http://h.test:99999/'])"
        "throws('TypeError',()=>new URL(bad));"
        "throws('TypeError',()=>new URL('x','not a base'));throws('TypeError',()=>new URL('x',null));"
                "throws('TypeError',()=>URL('http://h.test/'));throws('TypeError',()=>new URL());"
        "throws('TypeError',()=>Object.getOwnPropertyDescriptor(URL.prototype,'href').get.call({}));"
        "assert(URL.canParse('http://h.test/')===true&&URL.canParse('nope')===false&&URL.canParse('x','http://h.test/')===true);"
        "assert(URL.parse('nope')===null&&URL.parse('x','http://h.test/a').href==='http://h.test/x');"
        "assert(URL.parse.call(null,'http://h.test/') instanceof URL);"
        "class U2 extends URL{};assert(new U2('http://h.test/') instanceof U2);"
        "assert('a'.repeat(9000).length&&URL.canParse('http://h.test/'+'a'.repeat(9000))===false,'past OS64_URL_REF_MAX');"
        /* A query its URLSearchParams grew past what a parse takes is
         * held; a setter that would need the parse leaves it as it is. */
        "var big=new URL('http://h.test/a?x=1#f');big.searchParams.append('k','a'.repeat(9000));"
        "assert(big.href.length>9000&&big.hash==='#f'&&big.search.length>9000&&big.searchParams.get('x')==='1');"
        "var held=big.href;big.pathname='/p';big.hash='g';big.host='other.test';assert(big.href===held,'unchanged');"
        "big.searchParams.delete('k');assert(big.href==='http://h.test/a?x=1#f');"
        "var small=new URL('http://h.test/a');small.pathname='a'.repeat(9000);small.hash='b'.repeat(9000);"
        "small.search='c'.repeat(9000);assert(small.href==='http://h.test/a','a setter past the limit changes nothing');",
        "url: parsing, resolving and the components");
    dom_script(&f, STORAGE_ASSERT
        "var u=new URL('http://h.test:8080/p/q?a=1#f');"
        "u.search='b=2&c=3';assert(u.href==='http://h.test:8080/p/q?b=2&c=3#f');"
        "u.search='?x y';assert(u.search==='?x%20y');u.search='#?';assert(u.search==='?%23?'&&u.hash==='#f');"
        "u.search='';assert(u.href==='http://h.test:8080/p/q#f');u.search='?';assert(u.href==='http://h.test:8080/p/q?#f');"
        "u.hash='g';assert(u.hash==='#g');u.hash='';assert(u.href==='http://h.test:8080/p/q?');"
        "u.hash='#';assert(u.href==='http://h.test:8080/p/q?#');"
        "u.pathname='a?b#c';assert(u.pathname==='/a%3Fb%23c',u.pathname);u.pathname='';assert(u.pathname==='/');"
        "u.port='9090';assert(u.port==='9090');u.port='81abc';assert(u.port==='81');u.port='abc';assert(u.port==='81');"
        "u.port='99999';assert(u.port==='81');u.port='80';assert(u.port==='','a scheme\\'s own port is no port');"
        "u.port='81';u.port='';assert(u.port===''&&u.host==='h.test');"
        "u.host='other.test:81';assert(u.host==='other.test:81');u.host='third.test';assert(u.host==='third.test:81');"
        "u.hostname='fourth.test';assert(u.host==='fourth.test:81');u.hostname='x.test:5';assert(u.hostname==='fourth.test');"
        "u.hostname='';assert(u.hostname==='fourth.test');u.host='bad host';assert(u.hostname==='fourth.test');"
        "u.protocol='https';assert(u.protocol==='https:'&&u.port==='81');u.port='443';assert(u.port==='');"
        "u.protocol='foo';assert(u.protocol==='https:');u.protocol='1x';assert(u.protocol==='https:');"
        "u.username='me';u.password='pw';assert(u.username===''&&u.password===''&&u.href.indexOf('@')<0);"
        "u.href='http://new.test/n';assert(u.href==='http://new.test/n');"
        "throws('TypeError',()=>{u.href='not an address'});assert(u.href==='http://new.test/n');"
        "var o=new URL('mailto:a');o.pathname='b';assert(o.href==='mailto:a','an opaque path is not set');",
        "url: the setters");
    dom_script(&f, STORAGE_ASSERT
        "var u=new URL('http://h.test/?a=1&b=2&a=3');var p=u.searchParams;"
        "assert(p===u.searchParams&&p instanceof URLSearchParams&&p.get('a')==='1'&&p.getAll('a').join()==='1,3');"
        "p.append('c','x y');assert(u.search==='?a=1&b=2&a=3&c=x+y');"
        "p.delete('a');assert(u.search==='?b=2&c=x+y');p.delete('b','nope');assert(p.has('b'));p.delete('b','2');assert(!p.has('b'));"
        "u.search='?q=%41+b';assert(p.get('q')==='A b'&&p.size===1);"
        "p.delete('q');assert(u.href==='http://h.test/','an empty list takes the query away');"
        "u.href='http://h.test/?z=1&a=2';assert(p.get('z')==='1'&&p.size===2);"
        "var q=new URLSearchParams('?a=1&a=2&b=&=c&&d');"
        "assert(q.toString()==='a=1&a=2&b=&=c&d='&&q.size===5&&q.get('d')===''&&q.get('nope')===null);"
        "assert(new URLSearchParams({x:'1',y:'2'}).toString()==='x=1&y=2');"
        "assert(new URLSearchParams([['x','1'],['y','2']]).toString()==='x=1&y=2');"
        "assert(new URLSearchParams(new URLSearchParams('a=b')).toString()==='a=b');"
        "assert(new URLSearchParams(new Map([['m','n']])).toString()==='m=n');"
        "throws('TypeError',()=>new URLSearchParams([['x']]));throws('TypeError',()=>new URLSearchParams([['x','1','2']]));"
        "throws('TypeError',()=>new URLSearchParams([5]));throws('TypeError',()=>URLSearchParams());"
        "assert(new URLSearchParams({'\\ud835x':'1','xx':'2','\\ud83dx':'3'}).toString()==='%EF%BF%BDx=3&xx=2','a key the strings repeat is replaced');"
        "var s=new URLSearchParams('a b=c d&e~*-._=%7E&\\u00e9=\\ud83d\\ude00');"
        "assert(s.toString()==='a+b=c+d&e%7E*-._=%7E&%C3%A9=%F0%9F%98%80',s.toString());"
        "assert(new URLSearchParams('%zz=%4&x=%C3').toString()==='%25zz=%254&x=%EF%BF%BD');"
        "var t=new URLSearchParams('z=1&\\ufffd=2&\\ud83d\\ude00=3&a=4&z=0');t.sort();"
        "assert([...t.keys()].join()==='a,z,z,\\ud83d\\ude00,\\ufffd'&&t.getAll('z').join()==='1,0','sorted in UTF-16 code units, stably');"
        "var many=new URLSearchParams();for(var n=3000;n>0;n--)many.append('k'+(n%1000+1000),String(n));many.sort();"
        "var order=[...many];assert(order.length===3000&&order[0][0]==='k1000'&&order[0][1]==='3000'&&order[2][1]==='1000');"
        "for(var m=1;m<order.length;m++)if(order[m-1][0]>order[m][0]||(order[m-1][0]===order[m][0]&&+order[m-1][1]<+order[m][1]))"
        "assert(false,'many pairs sorted, stably, at '+m);"
        "t.set('z','9');assert(t.getAll('z').join()==='9'&&[...t.keys()].indexOf('z')===1);t.set('new','n');assert(t.get('new')==='n');"
        "var seen=[];t.forEach(function(v,k,o){seen.push(k+'='+v+(o===t)+(this===seen))},seen);assert(seen[0]==='a=4truetrue');"
        "var it=t.entries();assert(Object.prototype.toString.call(it)==='[object URLSearchParams Iterator]');"
        "assert(it.next().value.join()==='a,4');t.append('late','1');assert([...it].pop().join()==='late,1','live');"
        "assert(t[Symbol.iterator]===t.entries&&Object.getPrototypeOf(Object.getPrototypeOf(it))===Iterator.prototype);"
        "assert([...t.values()].length===t.size);"
        "throws('TypeError',()=>t.append('a'));throws('TypeError',()=>t.get());throws('TypeError',()=>t.forEach());"
        "throws('TypeError',()=>URLSearchParams.prototype.get.call({},'a'));"
        "assert(new URLSearchParams().toString()===''&&new URLSearchParams(undefined).size===0);",
        "url: URLSearchParams, and a URL's query live both ways");
    fixture_free(&f);
    static const char *const kOperations[] = {
        "new URL('http://h.test/p?a=1')", "new URL('x','http://h.test/a/')",
        "var u=new URL('http://h.test/');u.search='a=1'", "URL.canParse('http://h.test/')",
        "new URLSearchParams('a=1&b=2').toString()", "new URLSearchParams({a:'1'}).get('a')",
    };
    binding_sweep("URL", kOperations, sizeof(kOperations) / sizeof(kOperations[0]), NULL);
    static const char *const kEngine[] = {
        "var u=new URL('http://h.test/p?a=1&b=2#f');u.searchParams.append('c','3');u.hash='g';u.href",
        "var p=new URLSearchParams([['a','1'],['b','2']]);p.sort();[...p.entries()].join()",
        "var p=new URLSearchParams({x:'1'});p.forEach(function(){});p.set('x','2');p.toString()",
        "URL.parse('x','http://h.test/').origin",
    };
    engine_sweep("URL", kEngine, sizeof(kEngine) / sizeof(kEngine[0]), NULL);
}
