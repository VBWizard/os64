static void *canvas_font_alloc(void *opaque,size_t bytes){(void)opaque;return malloc(bytes);}
static void canvas_font_free(void *opaque,void *p,size_t bytes){(void)opaque;(void)bytes;free(p);}
typedef struct { os64_text_context_t *context;uint8_t *bytes;size_t length; } CanvasFonts;
static os64_font_status_t canvas_font_layout(void *opaque,const os64_font_family_list_t *families,
    bool bold,bool italic,uint32_t px,const uint8_t *text,size_t length,os64_text_run_t **out)
{
    (void)families;(void)bold;(void)italic;CanvasFonts *fonts=opaque;
    os64_font_face_options_t options={px,OS64_FONT_HINT_NONE};os64_text_font_t *font=NULL;
    os64_font_status_t status=os64_text_font_open(fonts->context,fonts->bytes,fonts->length,&options,&font);
    if(status!=OS64_FONT_OK)return status;
    os64_text_layout_t layout={.fonts=&font,.font_count=1,.tab_interval=64};
    status=os64_text_layout(fonts->context,text,length,&layout,out);os64_text_font_release(font);return status;
}
static void dom_canvas_paint_cases(void)
{
    DomFixture f=fixture_new(NULL);if(!fixture_ready(&f))return;
    dom_script(&f,STORAGE_ASSERT
        "var c=document.createElement('canvas');c.width=32;c.height=32;var x=c.getContext('2d');"
        "var g=x.createLinearGradient(0,0,32,0);assert(g instanceof CanvasGradient);"
        "throws('TypeError',()=>new CanvasGradient());throws('TypeError',()=>CanvasGradient());"
        "throws('TypeError',()=>x.createLinearGradient(0,0,1,Infinity));"
        "throws('TypeError',()=>x.createRadialGradient(0,0,0,0,NaN,2));"
        "throws('IndexSizeError',()=>x.createRadialGradient(0,0,-1,0,0,2));"
        "throws('IndexSizeError',()=>g.addColorStop(2,'red'));throws('SyntaxError',()=>g.addColorStop(0,'no-such-color'));"
        "throws('TypeError',()=>g.addColorStop(NaN,'red'));throws('TypeError',()=>g.addColorStop(0));"
        "var converted=false;throws('IndexSizeError',()=>g.addColorStop(2,{toString(){converted=true;return 'red'}}));assert(converted);"
        "g.addColorStop(0,'red');g.addColorStop(1,'blue');x.fillStyle=g;assert(x.fillStyle===g);"
        "x.fillRect(0,0,32,32);var p=x.getImageData(0,0,1,1).data;assert(p[0]===251&&p[2]===4&&p[3]===255);"
        "x.save();x.fillStyle='black';g.addColorStop(.5,'lime');x.restore();assert(x.fillStyle===g);"
        "x.fillRect(0,0,32,32);assert(x.getImageData(16,0,1,1).data[1]>240);"
        "x.strokeStyle=g;assert(x.strokeStyle===g);x.strokeStyle='blue';assert(x.strokeStyle==='#0000ff');"
        "var y=document.createElement('canvas').getContext('2d');y.fillStyle=g;assert(y.fillStyle===g);"
        "c.width=c.width;assert(x.fillStyle==='#000000');assert(y.fillStyle===g);"
        "x.scale(2,1);var h=x.createLinearGradient(0,0,16,0);h.addColorStop(0,'red');h.addColorStop(1,'blue');"
        "x.resetTransform();x.fillStyle=h;x.fillRect(0,0,32,32);assert(x.getImageData(24,0,1,1).data[2]===255);"
        "var cone=x.createRadialGradient(16,16,0,24,16,8);cone.addColorStop(0,'red');x.clearRect(0,0,32,32);"
        "x.fillStyle=cone;x.fillRect(0,0,32,32);assert(x.getImageData(2,16,1,1).data[3]===0);"
        "assert(x.getImageData(24,16,1,1).data[0]===255);"
        "var conic=x.createConicGradient(0,16.5,16.5);conic.addColorStop(0,'red');conic.addColorStop(.25,'lime');conic.addColorStop(1,'blue');"
        "x.fillStyle=conic;x.fillRect(0,0,32,32);assert(x.getImageData(16,24,1,1).data[1]===255);"
        "var current=x.createLinearGradient(0,0,1,0);current.addColorStop(0,'currentColor');x.fillStyle=current;x.fillRect(0,0,2,2);"
        "assert(x.getImageData(0,0,1,1).data[0]===0);"
        "throws('TypeError',()=>CanvasGradient.prototype.addColorStop.call({},0,'red'));"
        "throws('DataCloneError',()=>structuredClone(g));",
        "canvas: live gradient identity, saved references, colors, transform timing and radial cone");
    CanvasFonts fonts={0};FILE *file=fopen("userland/libfreetype/fixtures/DejaVuSans.ttf","rb");
    check(file!=NULL,"canvas font fixture");if(!file){fixture_free(&f);return;}
    fseek(file,0,SEEK_END);fonts.length=ftell(file);rewind(file);fonts.bytes=malloc(fonts.length);
    check(fonts.bytes&&fread(fonts.bytes,1,fonts.length,file)==fonts.length,"canvas font bytes");fclose(file);
    os64_text_options_t options={.memory={NULL,canvas_font_alloc,canvas_font_free},.backend=os64_freetype_backend_v1()};
    check(os64_text_create(&options,&fonts.context)==OS64_FONT_OK,"canvas text context");
    os64_dom_text_provider_t provider={canvas_font_layout,&fonts};os64_dom_set_text(f.binding,&provider);
    dom_script(&f,STORAGE_ASSERT
        "x.reset();c.width=160;c.height=80;assert(x.font==='10px sans-serif');"
        "assert(x.textAlign==='start'&&x.textBaseline==='alphabetic'&&x.direction==='inherit');"
        "x.font='24px serif';assert(x.font==='24px serif',x.font);x.font='nonsense';assert(x.font==='24px serif');"
        "x.save();x.font='italic bold 32px sans-serif';x.textAlign='center';x.textBaseline='middle';x.direction='rtl';x.restore();"
        "assert(x.font==='24px serif'&&x.textAlign==='start'&&x.textBaseline==='alphabetic'&&x.direction==='inherit');"
        "var m=x.measureText('AV');assert(m instanceof TextMetrics&&m.width>20&&m.actualBoundingBoxAscent>10);"
        "throws('TypeError',()=>new TextMetrics());throws('TypeError',()=>TextMetrics());"
        "var width=m.width;m.width=100;assert(m.width===width);"
        "x.scale(4,2);assert(x.measureText('AV').width===width);x.resetTransform();"
        "x.textAlign='right';var right=x.measureText('AV');assert(Math.abs(right.actualBoundingBoxLeft-m.actualBoundingBoxLeft-width)<.001);"
        "x.textAlign='start';x.direction='rtl';assert(x.measureText('AV').actualBoundingBoxLeft===right.actualBoundingBoxLeft);"
        "x.direction='ltr';assert(x.measureText('A\\tV').width===x.measureText('A V').width);"
        "assert(x.measureText('A\\nV').width===x.measureText('A V').width);"
        "assert(x.measureText('').width===0&&x.measureText('').actualBoundingBoxAscent===0);"
        "x.beginPath();x.rect(150,0,10,10);x.fillStyle='red';x.fillText('AV',10,40);"
        "var data=x.getImageData(0,0,160,80).data;assert(data.some((v,i)=>i%4===3&&v));"
        "assert(x.getImageData(155,5,1,1).data[3]===0);x.fillStyle='blue';x.fill();assert(x.getImageData(155,5,1,1).data[2]===255);"
        "x.clearRect(0,0,160,80);x.strokeStyle='lime';x.lineWidth=2;x.strokeText('AV',10,40);"
        "assert(x.getImageData(0,0,160,80).data.some((v,i)=>i%4===1&&v));"
        "x.clearRect(0,0,160,80);x.fillText('AV',10,40,0);assert(!x.getImageData(0,0,160,80).data.some(v=>v));"
        "x.fillText('AV',10,40,10);assert(!x.getImageData(22,0,130,80).data.some(v=>v));"
        "for(var method of ['fillText','strokeText']){"
        "x.clearRect(0,0,160,80);x[method]('AV',10,40);var expected=x.getImageData(0,0,160,80).data;"
        "assert(expected.some(v=>v));x.clearRect(0,0,160,80);x[method]('AV',10,40,Infinity);"
        "assert(x.getImageData(0,0,160,80).data.every((v,i)=>v===expected[i]),method+' Infinity');"
        "for(var bad of [NaN,0,-1,-Infinity]){x.clearRect(0,0,160,80);x[method]('AV',10,40,bad);"
        "assert(!x.getImageData(0,0,160,80).data.some(v=>v),method+' invalid maxWidth');}"
        "for(var bad of [NaN,Infinity,-Infinity]){x[method]('AV',bad,40,Infinity);x[method]('AV',10,bad,Infinity);}"
        "assert(!x.getImageData(0,0,160,80).data.some(v=>v),method+' invalid coordinates');}"
        "x.font='12pt sans-serif';assert(x.font==='16px sans-serif',x.font);"
        "x.font='2em serif';assert(x.font==='20px serif',x.font);"
        "x.font='calc(10px + 2px) serif';assert(x.font==='12px serif',x.font);"
        "c.width=c.width;assert(x.font==='10px sans-serif');"
        "x.font={toString(){c.width=c.width;return '30px serif'}};assert(x.font==='30px serif');"
        "x.fillText('A',{valueOf(){c.width=c.width;return 10}},40);assert(x.font==='10px sans-serif');"
        "throws('DataCloneError',()=>structuredClone(m));",
        "canvas: shared outline text, font state, metrics, whitespace, maxWidth, transforms and reentry");
    fixture_free(&f);check(os64_text_destroy(fonts.context)==OS64_FONT_OK,"canvas text released");free(fonts.bytes);
}
