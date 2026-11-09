static void dom_canvas_shadow_cases(void)
{
    DomFixture f=fixture_new(NULL);if(!fixture_ready(&f))return;
    dom_script(&f,STORAGE_ASSERT
        "var c=document.createElement('canvas');c.width=32;c.height=32;var x=c.getContext('2d');"
        "x.fillStyle='transparent';assert(x.shadowColor===x.fillStyle);"
        "assert(x.shadowBlur===0&&x.shadowOffsetX===0&&x.shadowOffsetY===0);"
        "x.shadowColor='blue';x.shadowOffsetX=8;x.shadowOffsetY=-2;x.shadowBlur=6.5;x.save();"
        "x.shadowColor='red';x.shadowOffsetX=-10;x.shadowBlur=0;x.restore();"
        "assert(x.shadowColor==='#0000ff'&&x.shadowOffsetX===8&&x.shadowOffsetY===-2&&x.shadowBlur===6.5);"
        "for(var v of [NaN,Infinity,-Infinity]){x.shadowBlur=v;x.shadowOffsetX=v;x.shadowOffsetY=v}"
        "x.shadowBlur=-1;x.shadowColor='not-a-color';"
        "assert(x.shadowColor==='#0000ff'&&x.shadowOffsetX===8&&x.shadowOffsetY===-2&&x.shadowBlur===6.5);"
        "x.shadowColor={toString(){c.width=c.width;return 'red'}};"
        "assert(x.shadowColor==='#ff0000'&&x.shadowOffsetX===0&&x.shadowBlur===0);"
        "x.shadowOffsetX={valueOf(){c.width=c.width;return 8}};"
        "assert(x.shadowOffsetX===8&&x.shadowBlur===0);x.shadowColor='blue';x.fillStyle='red';"
        "x.fillRect(-4,2,2,2);assert(x.getImageData(4,2,1,1).data.join()==='0,0,255,255');"
        "x.clearRect(4,2,2,2);assert(x.getImageData(4,2,1,1).data[3]===0);"
        "var im=x.createImageData(1,1);im.data[0]=255;im.data[3]=255;x.globalAlpha=0;"
        "x.putImageData(im,2,2);assert(x.getImageData(2,2,1,1).data.join()==='255,0,0,255');"
        "assert(x.getImageData(10,2,1,1).data[3]===0);"
        "c.width=c.width;assert(x.shadowOffsetX===0&&x.shadowBlur===0);"
        "x.fillStyle='transparent';assert(x.shadowColor===x.fillStyle);"
        "x.shadowBlur=3;x.shadowOffsetY=4;x.shadowColor='blue';x.reset();"
        "assert(x.shadowBlur===0&&x.shadowOffsetY===0);"
        "throws('TypeError',()=>Object.getOwnPropertyDescriptor(CanvasRenderingContext2D.prototype,'shadowBlur').get.call({}));",
        "canvas: shadow attributes, invalid assignments, reentry, reset and pixel-transfer isolation");
    fixture_free(&f);
}
