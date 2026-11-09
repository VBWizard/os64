static void svg_fill_hit_cases(void)
{
    probe_page("<!doctype html><style>svg{display:block} .hidden{display:none} #box{width:20px;height:20px}"
        "#rules{fill-rule:evenodd}</style><p id=result>pending</p>"
        "<svg width=200 height=100 viewBox='0 0 100 50'><g id=rules>"
        "<path id=path fill=none transform='scale(4 1)' d='M0 0H40V40H0Z M10 10H30V30H10Z'/></g>"
        "<rect id=box x=50 y=5 width=80 height=80/>"
        "<circle id=circle cx='50%' cy='50%' r='10'/>"
        "<defs><path id=hidden style='display:none;filter:blur(8px);opacity:0' d='M0 0H20V20H0Z'/></defs>"
        "<svg width=40 height=40 viewBox='0 0 10 10'><ellipse id=ellipse cx='50%' cy='50%' rx=3 ry='2'/></svg>"
        "<polyline id=triangle points='0,0 10,0 0,10'/><line id=line x1=0 y1=0 x2=10 y2='10'/></svg>"
        "<svg width=100 height=100 style='padding:20px;border:5px solid red'><circle id=padded cx='50%' cy='50%' r='10'/></svg>"
        "<script>try{function ok(v){if(!v)throw Error('fill assertion '+(++step));step++}var step=0;"
        "var by=id=>document.getElementById(id),p=by('path');"
        "ok(p.isPointInFill({x:5,y:5}));ok(!p.isPointInFill({x:20,y:20}));ok(p.isPointInFill({x:10,y:20}));"
        "by('rules').style.fillRule='nonzero';ok(p.isPointInFill({x:20,y:20}));"
        "ok(!p.isPointInFill({x:80,y:20}));ok(by('hidden').isPointInFill(new DOMPoint(10,10)));"
        "ok(by('box').isPointInFill({x:60,y:10}));ok(!by('box').isPointInFill({x:80,y:10}));"
        "ok(by('circle').isPointInFill({x:50,y:25}));ok(!by('circle').isPointInFill({x:50,y:40}));"
        "ok(by('ellipse').isPointInFill({x:5,y:5}));ok(!by('ellipse').isPointInFill({x:9,y:5}));"
        "ok(by('triangle').isPointInFill({x:2,y:2}));ok(!by('triangle').isPointInFill({x:8,y:8}));"
        "ok(by('line').isPointInFill({x:5,y:5}));ok(!by('line').isPointInFill({x:5,y:6}));"
        "ok(!p.isPointInFill({get x(){p.setAttribute('d','M0 0H2V2H0Z');return 5},y:5}));"
        "var detached=document.createElementNS('http://www.w3.org/2000/svg','path');"
        "detached.setAttribute('d','M0 0H10V10H0Z');function refused(n){try{n.isPointInFill({x:5,y:5})}catch(e){return e.name==='InvalidStateError'}return false}ok(refused(detached));"
        "var orphan=document.createElementNS('http://www.w3.org/2000/svg','circle');"
        "orphan.setAttribute('cx','50');orphan.setAttribute('cy','25');orphan.setAttribute('r','10');ok(refused(orphan));document.body.appendChild(orphan);ok(orphan.isPointInFill({x:50,y:25}));"
        "orphan.setAttribute('cx','50%');ok(refused(orphan));"
        "ok(by('padded').isPointInFill({x:50,y:50}));ok(!by('padded').isPointInFill({x:75,y:75}));"
        "by('result').textContent='passed '+step;}catch(e){document.getElementById('result').textContent='failed '+step+' '+e.name+' '+e.message}</script>",true);
    for(unsigned i=0;i<20;i++)if(!script_turn())break;
    if(!probe_text_is("result","passed 23"))fprintf(stderr,"SVG hit console: %s result %s\n",yonder_host_console,probe_text_of("result"));
    check(probe_text_is("result","passed 23"),"SVG fill query: live styles, mutation, local units, hidden geometry, shapes and viewports");
    probe_drop();
}
