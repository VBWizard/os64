static os64_geometry_status_t dom_hit_provider(void *opaque,const os64_html_node_t *node,
    double x,double y,const os64_geometry_memory_t *memory,bool (*cancelled)(void *),void *cancel_context,
    bool *inside,os64_dom_geometry_stats_t *cost)
{
    (void)opaque;(void)memory;(void)cancelled;(void)cancel_context;
    const os64_html_attr_t *a=os64_html_attr(node,"data-test");
    *inside=a&&a->value&&strcmp(a->value,"fresh")==0&&x==2&&y==3;
    *cost=(os64_dom_geometry_stats_t){0};return OS64_GEOMETRY_OK;
}
static void dom_svg_hit_cases(void)
{
    DomFixture f=fixture_new(NULL);if(!fixture_ready(&f))return;
    os64_dom_set_svg_hit(f.binding,dom_hit_provider,NULL);
    dom_script(&f,STORAGE_ASSERT
        "var p=new DOMPoint(2,3);assert(p.x===2&&p.y===3&&p.z===0&&p.w===1);"
        "assert(p instanceof DOMPointReadOnly&&p instanceof DOMPoint);"
        "assert(Object.getPrototypeOf(DOMPoint)===DOMPointReadOnly);"
        "throws('TypeError',()=>DOMPoint());class Child extends DOMPoint{};assert(new Child() instanceof Child);"
        "p.x='4';assert(p.x===4);var ro=DOMPointReadOnly.fromPoint(p);p.x=8;assert(ro.x===4);"
        "assert(JSON.stringify(ro)==='{" "\"x\":4,\"y\":3,\"z\":0,\"w\":1}');"
        "var order='';DOMPoint.fromPoint({get x(){order+='x';return 1},get y(){order+='y';return 2},get z(){order+='z';return 3},get w(){order+='w';return 4}});assert(order==='wxyz');"
        "assert(DOMPoint.fromPoint(null).w===1);throws('TypeError',()=>DOMPoint.fromPoint(3));"
        "assert(new DOMPoint(NaN,Infinity).y===Infinity);"
        "var t=new DOMPoint(2,3).matrixTransform({a:2,d:3,e:10,f:20});assert(t.x===14&&t.y===29&&t.w===1);"
        "throws('TypeError',()=>p.matrixTransform({a:2,m11:3}));throws('TypeError',()=>p.matrixTransform({is2D:true,m13:2}));"
        "var path=document.createElementNS('http://www.w3.org/2000/svg','path');"
        "assert(path instanceof SVGGeometryElement&&path instanceof SVGGraphicsElement&&path instanceof SVGElement);"
        "assert(!('isPointInFill' in document.body));assert(!('isPointInFill' in document.createElementNS('http://www.w3.org/2000/svg','g')));"
        "assert(path.isPointInFill({get x(){path.setAttribute('data-test','fresh');return 2},y:3}));"
        "assert(path.isPointInFill(new DOMPoint(2,3)));assert(!path.isPointInFill());"
        "assert(!path.isPointInFill({x:NaN,y:3}));assert(!path.isPointInFill({x:Infinity,y:3}));"
        "throws('TypeError',()=>path.isPointInFill(2));"
        "throws('TypeError',()=>SVGGeometryElement.prototype.isPointInFill.call(document.body,{}));"
        "throws('TypeError',()=>new SVGGeometryElement());"
        "for(var i=0;i<20000;i++){var q=new DOMPoint(i,3);if(q.x!==i)throw Error('point');}",
        "DOMPoint and SVG fill query: brands, ownership, coercion, dictionaries and transforms");
    fixture_free(&f);
}
