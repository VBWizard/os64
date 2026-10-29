#include <dom/canvas.h>
static unsigned canvas_image_loans,canvas_image_returns;
static os64_dom_image_status_t canvas_test_image(void *opaque,const os64_html_node_t *node,bool pixels,
                                                os64_dom_image_view_t *out)
{
    (void)opaque;(void)pixels;
    static uint32_t colors[]={0xffff0000,0xff00ff00,0xff0000ff,0xffffffff};
    const os64_html_attr_t *src=os64_html_attr(node,"src");
    *out=(os64_dom_image_view_t){0};
    if(!src||!strcmp(src->value,"pending"))return OS64_DOM_IMAGE_PENDING;
    if(!strcmp(src->value,"broken"))return OS64_DOM_IMAGE_BROKEN;
    *out=(os64_dom_image_view_t){{colors,2,2,8,sizeof(colors),DRAW2D_ARGB8_STRAIGHT},strcmp(src->value,"tainted")!=0,(void *)colors};
    canvas_image_loans++;return OS64_DOM_IMAGE_READY;
}
static void canvas_test_return(void *opaque,os64_dom_image_view_t *view)
{ (void)opaque;if(view->token)canvas_image_returns++; }
static void dom_canvas_image_cases(void)
{
    DomFixture f=fixture_new(NULL);if(!fixture_ready(&f))return;
    os64_dom_image_provider_t provider={canvas_test_image,canvas_test_return,NULL};
    os64_dom_set_images(f.binding,&provider);canvas_image_loans=canvas_image_returns=0;
    dom_script(&f,STORAGE_ASSERT
        "var source=document.createElement('canvas');source.width=2;source.height=2;"
        "var s=source.getContext('2d');s.fillStyle='red';s.fillRect(0,0,1,2);s.fillStyle='lime';s.fillRect(1,0,1,2);"
        "var c=document.createElement('canvas');c.width=8;c.height=8;var d=c.getContext('2d');"
        "assert(d.imageSmoothingEnabled&&d.imageSmoothingQuality==='low');d.imageSmoothingEnabled=false;"
        "d.save();d.imageSmoothingQuality='high';d.imageSmoothingEnabled=true;d.restore();"
        "assert(!d.imageSmoothingEnabled&&d.imageSmoothingQuality==='low');"
        "d.drawImage(source,0,0,8,8);assert(d.getImageData(0,0,1,1).data.join(',')==='255,0,0,255');"
        "assert(d.getImageData(7,0,1,1).data.join(',')==='0,255,0,255');"
        "d.drawImage(c,0,0,6,8,2,0,6,8);assert(d.getImageData(5,0,1,1).data[0]===255);"
        "var img=new Image();assert(img.complete&&img.naturalWidth===0&&img.crossOrigin===null);"
        "img.crossOrigin='ANONYMOUS';assert(img.crossOrigin==='anonymous');img.crossOrigin=null;assert(img.crossOrigin===null);"
        "img.src='clean';assert(img.complete&&img.naturalWidth===2&&img.naturalHeight===2);"
        "d.drawImage(img,0,0);assert(d.getImageData(0,1,1,1).data.join(',')==='0,0,255,255');"
        "var url=c.toDataURL('image/not-supported',{valueOf(){throw Error('quality must not be coerced')}});"
        "assert(url.startsWith('data:image/png;base64,'));var png=atob(url.slice(22));"
        "assert(png.charCodeAt(0)===137&&png.slice(1,4)==='PNG'&&png.charCodeAt(19)===8&&png.charCodeAt(23)===8);"
        "var blank=document.createElement('canvas');blank.width=1;blank.height=1;assert(blank.toDataURL().startsWith('data:image/png'));"
        "assert(blank.getContext('2d').getImageData(0,0,1,1).data.every(x=>x===0));"
        "blank.width=0;assert(blank.toDataURL()==='data:,');throws('InvalidStateError',()=>d.drawImage(blank,0,0));"
        "img.src='pending';assert(!img.complete);d.drawImage(img,0,0);img.src='broken';"
        "throws('InvalidStateError',()=>d.drawImage(img,0,0));d.drawImage(img,NaN,0);"
        "img.src='tainted';d.drawImage(img,0,0);throws('SecurityError',()=>c.toDataURL());"
        "throws('SecurityError',()=>d.getImageData(0,0,1,1));"
        "s.drawImage(c,0,0);throws('SecurityError',()=>source.toDataURL());"
        "d.save();d.restore();throws('SecurityError',()=>c.toDataURL());"
        "c.width=c.width;assert(c.toDataURL().startsWith('data:image/png'));"
        "d.scale(0,0);d.drawImage(img,0,0);throws('SecurityError',()=>c.toDataURL());d.reset();"
        "img.src='tainted';d.drawImage(img,{valueOf(){img.src='clean';c.width=4;return 0}},0);"
        "assert(c.toDataURL().startsWith('data:image/png'));"
        "d.drawImage(img,Infinity,{valueOf(){c.width=3;return 0}});assert(c.width===3);"
        "throws('TypeError',()=>d.drawImage({},0,0));throws('TypeError',()=>d.drawImage(img,0,0,1));",
        "canvas images: sources, overloads, self-copy, conversion reentrancy, smoothing, taint and PNG export");
    check(canvas_image_loans==canvas_image_returns,"canvas image loans released");
    fixture_free(&f);
}
