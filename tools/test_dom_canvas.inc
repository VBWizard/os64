/* ImageData owns its typed array; native pixel tests live in test_canvas_core.c. */
static void dom_image_data_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, STORAGE_ASSERT
        "var image=new ImageData(2,3);"
        "assert(image.width===2&&image.height===3&&image.data.length===24);"
        "assert(image.data instanceof Uint8ClampedArray&&image.data.every(x=>x===0));"
        "assert(image.colorSpace==='srgb'&&image.pixelFormat==='rgba-unorm8');"
        "assert(image instanceof ImageData&&Object.prototype.toString.call(image)==='[object ImageData]');"
        "assert(ImageData.length===2);"
        "image.data[0]=300;image.data[1]=-1;assert(image.data[0]===255&&image.data[1]===0);"
        "var same=image.data;image.width=99;image.height=99;image.data=[];"
        "assert(image.width===2&&image.height===3&&image.data===same);"
        "var all=new Uint8ClampedArray(24),sub=all.subarray(4,20);"
        "var supplied=new ImageData(sub,2);assert(supplied.data===sub&&supplied.height===2);"
        "all[4]=77;assert(supplied.data[0]===77);"
        "var sized=new ImageData(sub,2,2);assert(sized.data===sub);"
        "class DerivedImage extends ImageData{};assert(new DerivedImage(1,1) instanceof DerivedImage);"
        "throws('TypeError',()=>ImageData(1,1));throws('TypeError',()=>new ImageData(1));"
        "throws('IndexSizeError',()=>new ImageData(0,1));throws('IndexSizeError',()=>new ImageData(1,0));"
        "throws('InvalidStateError',()=>new ImageData(new Uint8ClampedArray(0),1));"
        "throws('InvalidStateError',()=>new ImageData(new Uint8ClampedArray(3),1));"
        "throws('IndexSizeError',()=>new ImageData(sub,3));"
        "throws('IndexSizeError',()=>new ImageData(sub,2,3));"
        "throws('IndexSizeError',()=>new ImageData(sub,0));"
        "throws('TypeError',()=>new ImageData(new Uint8Array(4),1));"
        "throws('TypeError',()=>new ImageData(1,1,{colorSpace:'garbage'}));"
        "throws('TypeError',()=>new ImageData(1,1,{pixelFormat:'garbage'}));"
        "throws('NotSupportedError',()=>new ImageData(1,1,{colorSpace:'display-p3'}));"
        "throws('NotSupportedError',()=>new ImageData(1,1,{pixelFormat:'rgba-float16'}));"
        "throws('TypeError',()=>Object.getOwnPropertyDescriptor(ImageData.prototype,'data').get.call({}));"
        "var order=[];new ImageData({valueOf(){order.push('w');return 1}},"
          "{valueOf(){order.push('h');return 1}},"
          "{get colorSpace(){order.push('c');return 'srgb'},get pixelFormat(){order.push('p');return 'rgba-unorm8'}});"
        "assert(order.join('')==='whcp',order);"
        "var detached=new Uint8ClampedArray(4);structuredClone(detached.buffer,{transfer:[detached.buffer]});"
        "throws('InvalidStateError',()=>new ImageData(detached,1));"
        "var changing=new Uint8ClampedArray(4);"
        "throws('InvalidStateError',()=>new ImageData(changing,{valueOf(){"
          "structuredClone(changing.buffer,{transfer:[changing.buffer]});return 1}}));"
        "var owned=new ImageData(1,1),held=owned.data;"
        "structuredClone(held.buffer,{transfer:[held.buffer]});assert(owned.width===1&&owned.data===held);"
        "var rab=new ArrayBuffer(4,{maxByteLength:8});"
        "throws('TypeError',()=>new ImageData(new Uint8ClampedArray(rab),1));"
        "var spoof=new Uint8ClampedArray(4);Object.defineProperty(spoof,'buffer',{get(){throw Error('spoof')}});"
        "assert(new ImageData(spoof,1).data===spoof);"
        "var original=new ImageData(2,1);original.data[0]=37;"
        "var copy=structuredClone({image:original,again:original,array:original.data});"
        "assert(copy.image instanceof ImageData&&copy.image!==original&&copy.image===copy.again);"
        "assert(copy.image.data===copy.array&&copy.array!==original.data&&copy.array[0]===37);"
        "assert(copy.image.width===2&&copy.image.height===1);"
        "var moved=structuredClone(original,{transfer:[original.data.buffer]});"
        "assert(moved.data[0]===37&&original.data.length===0);"
        "throws('DataCloneError',()=>structuredClone(original));"
        "var cyclic=new ImageData(1,1);cyclic.data.owner=cyclic;cyclic=null;",
        "canvas: ImageData ownership, conversion, dimensions, settings and detach");
    fixture_free(&f);
}

static void dom_canvas_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, STORAGE_ASSERT
        "var c=document.createElement('canvas');c.id='canvas-reset';document.body.append(c);"
        "assert(c.width===300&&c.height===150);"
        "assert(c.getContext('webgl')===null);"
        "var ctx=c.getContext('2d');assert(ctx===c.getContext('2d')&&ctx.canvas===c);"
        "assert(ctx instanceof CanvasRenderingContext2D);"
        "throws('TypeError',()=>new CanvasRenderingContext2D());"
        "throws('TypeError',()=>ctx.fillRect.call({},0,0,1,1));"
        "c.width=4;c.height=3;ctx.fillStyle='red';ctx.fillRect(0,0,4,3);"
        "assert(ctx.getImageData(1,1,1,1).data.join(',')==='255,0,0,255');"
        "ctx.globalAlpha=.5;ctx.save();ctx.fillStyle='#0000ff';ctx.fillRect(0,0,1,1);ctx.restore();"
        "assert(ctx.fillStyle==='#ff0000'&&ctx.globalAlpha===.5);"
        "ctx.clearRect(1,1,1,1);assert(ctx.getImageData(1,1,1,1).data.every(x=>x===0));"
        "var saved=ctx.getImageData(0,0,4,3);ctx.clearRect(0,0,4,3);ctx.putImageData(saved,0,0);"
        "assert(ctx.getImageData(0,0,4,3).data.join(',')===saved.data.join(','));"
        "c.width=c.width;assert(ctx===c.getContext('2d')&&ctx.fillStyle==='#000000'&&ctx.globalAlpha===1);"
        "assert(ctx.getImageData(0,0,4,3).data.every(x=>x===0));ctx.restore();assert(ctx.globalAlpha===1);"
        "ctx.fillStyle='lime';ctx.fillRect(0,0,1,1);c.remove();document.body.append(c);"
        "assert(ctx.getImageData(0,0,1,1).data.join(',')==='0,255,0,255');"
        "c.style.width='400px';assert(ctx.getImageData(0,0,1,1).data[1]===255);"
        "ctx.fillRect({valueOf(){c.width=c.width;return 0}},0,1,1);"
        "assert(ctx.getImageData(0,0,1,1).data.join(',')==='0,0,0,255');"
        "ctx.fillStyle={toString(){c.height=c.height;return 'red'}};assert(ctx.fillStyle==='#ff0000');"
        "ctx.fillStyle='rgba(255,255,255,0.5)';ctx.save();ctx.fillStyle='red';ctx.restore();"
        "assert(ctx.fillStyle==='rgba(255, 255, 255, 0.5)');ctx.fillStyle='red';"
        "ctx.globalAlpha=-1;assert(ctx.globalAlpha===1);ctx.globalAlpha=NaN;assert(ctx.globalAlpha===1);"
        "ctx.fillStyle='invalid';assert(ctx.fillStyle==='#ff0000');"
        "var im=ctx.createImageData(-2,-2);assert(im.width===2&&im.height===2);"
        "assert(ctx.createImageData(im).data!==im.data);"
        "im.data.set([255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,255]);"
        "ctx.clearRect(0,0,4,3);ctx.putImageData(im,0,0,2,2,-1,-1);"
        "assert(ctx.getImageData(1,1,1,1).data.join(',')==='255,255,255,255');"
        "assert(ctx.getImageData(0,0,1,1).data.every(x=>x===0));"
        "assert(ctx.getImageData(-1,-1,1,1).data.every(x=>x===0));"
        "throws('IndexSizeError',()=>ctx.getImageData(0,0,0,1));"
        "throws('TypeError',()=>ctx.getImageData(NaN,0,1,1));"
        "throws('TypeError',()=>ctx.putImageData(im,Infinity,0));"
        "throws('TypeError',()=>ctx.putImageData(im,0,0,1));"
        "throws('TypeError',()=>ctx.putImageData(im,0,0,1,1));"
        "throws('TypeError',()=>ctx.putImageData(im,0,0,1,1,1));"
        "throws('InvalidStateError',()=>ctx.putImageData(im,{valueOf(){"
          "structuredClone(im.data.buffer,{transfer:[im.data.buffer]});return 0}},0));"
        "c.width=20000;assert(ctx.isContextLost());assert(ctx.getImageData(0,0,1,1).data.every(x=>x===0));"
        "c.width=4;assert(!ctx.isContextLost()&&ctx===c.getContext('2d'));"
        "ctx.fillStyle='red';ctx.fillRect(0,0,1,1);",
        "canvas: context identity, pixels, conversion reentrancy and dimension resets");
    /* Native attribute writes take the same route as script writes. */
    os64_html_node_t *canvas = native_id(f.document->document, "canvas-reset");
    uint64_t version = os64_html_version(f.document);
    check(os64_html_set_attr(f.document, canvas, "width", "4", 1) == OS64_HTML_OK &&
          os64_html_version(f.document) == version, "native same-value canvas assignment");
    dom_script(&f, STORAGE_ASSERT
        "assert(ctx.fillStyle==='#000000');assert(ctx.getImageData(0,0,1,1).data.every(x=>x===0));",
        "canvas: native same-value reset reaches binding listener");
    fixture_free(&f);
}

static void dom_canvas_refusals(void)
{
    for (size_t cut=1;cut<=12;++cut) {
        DomFixture f=fixture_new(NULL);
        if (!fixture_ready(&f)) return;
        dom_script(&f,"var c=document.createElement('canvas');c.width=4;c.height=4;", "canvas refusal setup");
        dom_fail_at=dom_attempts+cut;
        os64_js_outcome_t outcome;
        const char *source="try{var x=c.getContext('2d');if(x){x.save();c.width=8;x.fillRect(0,0,1,1);"
                            "x.restore();x.getImageData(0,0,1,1)}}catch(e){}";
        check(os64_js_run(f.runtime,source,strlen(source),"canvas allocation refusal",&outcome)==OS64_JS_OK,
              "Canvas allocation refusal leaves a usable script runtime");
        dom_fail_at=0;
        dom_script(&f,STORAGE_ASSERT "var x=c.getContext('2d');c.width=4;assert(x&&!x.isContextLost());",
                   "Canvas recovers on a later resize or context creation");
        fixture_free(&f);
    }
}
