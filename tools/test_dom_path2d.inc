static void dom_path2d_cases(void)
{
    DomFixture f=fixture_new(NULL);if(!fixture_ready(&f))return;
    dom_script(&f,STORAGE_ASSERT
        "var c=document.createElement('canvas');c.width=64;c.height=64;var x=c.getContext('2d');"
        "var path=new Path2D('M2 2h8v8h-8z'),copy=new Path2D(path);"
        "assert(path instanceof Path2D&&copy!==path&&copy instanceof Path2D);"
        "assert(Path2D.length===0);throws('TypeError',()=>Path2D());"
        "class Child extends Path2D{};assert(new Child(path) instanceof Child);"
        "var red=(a,b)=>x.getImageData(a,b,1,1).data[0],alpha=(a,b)=>x.getImageData(a,b,1,1).data[3];"
        "x.fillStyle='red';x.beginPath();x.rect(40,40,8,8);x.translate(12,0);x.fill(copy);"
        "assert(red(16,4)===255&&alpha(4,4)===0&&alpha(44,44)===0);"
        "x.resetTransform();x.fill();assert(red(44,44)===255);"
        "c.width=c.width;x.fillStyle='red';x.fill(path);assert(red(4,4)===255);"
        "x.reset();x.save();x.translate(12,0);x.clip(path);x.resetTransform();x.fillRect(0,0,64,64);"
        "assert(alpha(16,4)===255&&alpha(4,4)===0&&alpha(44,44)===0);"
        "x.restore();x.fillRect(40,40,8,8);assert(alpha(44,44)===255);"
        "x.reset();x.strokeStyle='red';x.lineWidth=2;x.translate(12,0);x.stroke(path);"
        "assert(red(16,2)===255&&alpha(16,6)===0);"
        "x.reset();var hole=new Path2D('M2 2h24v24h-24z M8 8h12v12h-12z');"
        "x.fill(hole,'evenodd');assert(alpha(4,4)===255&&alpha(12,12)===0);"
        "x.reset();x.fill(hole);assert(alpha(12,12)===255);"
        "x.reset();x.fill(new Path2D('M2 2h8v8h-8z Lbad'));assert(alpha(4,4)===255);"
        "x.reset();x.fill(new Path2D('not a path'));assert(alpha(4,4)===0);"
        "x.fill(new Path2D('M2 2h8v8h-8z\\u0000M20 20h8v8h-8z'));"
        "assert(alpha(4,4)===255&&alpha(24,24)===0);"
        "x.reset();var arc=new Path2D('M4 12 A8 8 0 1 0 20 12 A8 8 0 1 0 4 12 Z');"
        "x.scale(2,2);x.fill(arc);assert(alpha(24,24)===255&&alpha(5,5)===0);"
        "x.reset();var text={toString(){c.width=c.width;return 'M2 2h8v8h-8z'}};"
        "x.fill(new Path2D(text));assert(alpha(4,4)===255);"
        "x.translate(20,0);x.fill(path,{toString(){c.width=c.width;return 'nonzero'}});"
        "assert(alpha(4,4)===255&&alpha(24,4)===0);"
        "throws('TypeError',()=>x.fill(path,'bad'));throws('TypeError',()=>x.stroke({}));"
        "throws('TypeError',()=>CanvasRenderingContext2D.prototype.fill.call(path,copy));"
        "throws('QuotaExceededError',()=>new Path2D('M'+ '1 '.repeat(40000)));"
        "x.reset();x.shadowColor='blue';x.shadowOffsetX=20;x.fill(path);"
        "assert(alpha(4,4)===255&&x.getImageData(24,4,1,1).data[2]===255);",
        "Path2D: SVG prefix, copy, transforms, fill rules, clipping, shadows, reentry and limits");
    fixture_free(&f);
    for(size_t cut=1;cut<=128;cut++) {
        f=fixture_new(NULL);if(!fixture_ready(&f))return;
        dom_script(&f,"var c=document.createElement('canvas');c.width=32;c.height=32;var x=c.getContext('2d');x.fillStyle='green';x.fillRect(0,0,32,32);x.fillStyle='red';", "Path2D refusal setup");
        size_t start=dom_attempts;dom_fail_at=start+cut;
        os64_js_outcome_t outcome;
        const char *source="var failed=false;try{x.fill(new Path2D(new Path2D('M2 2A8 8 0 0 1 18 2L18 18L2 18Z')))}catch(e){failed=true}";
        check(os64_js_run(f.runtime,source,strlen(source),"Path2D allocation refusal",&outcome)==OS64_JS_OK,"Path2D refusal is catchable");
        size_t attempts=dom_attempts-start;dom_fail_at=0;
        dom_script(&f,STORAGE_ASSERT "if(failed)assert(x.getImageData(4,4,1,1).data[0]===0);x.fill(new Path2D('M2 2h8v8h-8z'));assert(x.getImageData(4,4,1,1).data[0]===255);", "Path2D refusal preserves pixels and permits recovery");
        fixture_free(&f);
        if(cut>attempts)break;
        check(cut<128,"Path2D refusal sweep reaches successful allocation");
    }
}
