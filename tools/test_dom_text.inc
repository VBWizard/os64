/* btoa and atob (libdom/base64.c), TextEncoder and TextDecoder
 * (libdom/encoding.c): a case per sentence of each file's contract, and
 * the binding's allocations failed in turn. */

static void dom_base64_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, STORAGE_ASSERT
        "assert(btoa('')===''&&btoa('f')==='Zg=='&&btoa('fo')==='Zm8='&&btoa('foo')==='Zm9v');"
        "assert(btoa('\\u0000\\u00ff')==='AP8='&&btoa(undefined)==='dW5kZWZpbmVk'&&btoa(null)==='bnVsbA==');"
        "var all='';for(var i=0;i<256;i++)all+=String.fromCharCode(i);"
        "assert(atob(btoa(all))===all,'every byte round-trips');"
        "throws('InvalidCharacterError',()=>btoa('\\u0100'));throws('InvalidCharacterError',()=>btoa('a\\ud800'));"
        "throws('InvalidCharacterError',()=>btoa('\\u20ac'));"
        "throws('TypeError',()=>btoa());throws('TypeError',()=>atob());throws('TypeError',()=>btoa(Symbol()));"
        "assert(atob('Zm9v')==='foo'&&atob(' Z m\\t9\\nv\\f\\r')==='foo'&&atob('Zg')==='f'&&atob('Zg==')==='f');"
        "assert(atob('Zm8')==='fo'&&atob('Zm8=')==='fo'&&atob('')===''&&atob('A/8=')==='\\u0003\\u00ff');"
        "assert(atob('Zh==')==='f','discarded bits are dropped');"
        "for(const bad of ['Z','Zm9vZ','Zg=','Zg===','====','ab*c','Zm9v=','\\u00e9AAA','Z=g=','=Zm9'])"
        "throws('InvalidCharacterError',()=>atob(bad));"
        "throws('InvalidCharacterError',()=>atob(undefined));",
        "base64: btoa and atob");
    fixture_free(&f);
}

static void dom_encoding_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, STORAGE_ASSERT
        "function bytes(u){return Array.prototype.join.call(u,',')}"
        "var e=new TextEncoder();assert(e.encoding==='utf-8'&&e instanceof TextEncoder);"
        "assert(Object.prototype.toString.call(e)==='[object TextEncoder]');"
        "assert(e.encode() instanceof Uint8Array&&e.encode().length===0&&e.encode(undefined).length===0);"
        "assert(bytes(e.encode('a\\u20ac\\ud83d\\ude00'))==='97,226,130,172,240,159,152,128');"
        "assert(bytes(e.encode('\\ud800x\\udc00'))==='239,191,189,120,239,191,189','a lone surrogate is U+FFFD');"
        "assert(bytes(e.encode('\\u0000'))==='0'&&bytes(e.encode(5))==='53');"
        "throws('TypeError',()=>TextEncoder.prototype.encode.call({},'a'));"
        "throws('TypeError',()=>TextEncoder());"
        /* encodeInto: as much as fits, never part of a character. */
        "var into=new Uint8Array(5);var r=e.encodeInto('a\\ud83d\\ude00b',into);"
        "assert(r.read===3&&r.written===5&&bytes(into)==='97,240,159,152,128',JSON.stringify(r));"
        "into=new Uint8Array(4);r=e.encodeInto('a\\ud83d\\ude00',into);assert(r.read===1&&r.written===1);"
        "var big=new Uint8Array(10);var sub=big.subarray(4,7);r=e.encodeInto('\\u00a5\\u00a5',sub);"
        "assert(r.read===1&&r.written===2&&big[4]===0xc2&&big[5]===0xa5&&big[6]===0,'into a view at an offset');"
        "r=e.encodeInto(undefined,new Uint8Array(9));assert(r.read===9&&r.written===9,'undefined is a string');"
        "throws('TypeError',()=>e.encodeInto('a',new Uint16Array(4)));throws('TypeError',()=>e.encodeInto('a'));"
        "throws('TypeError',()=>e.encodeInto('a',new ArrayBuffer(4)));",
        "encoding: TextEncoder");
    dom_script(&f, STORAGE_ASSERT
        "function u8(a){return new Uint8Array(a)}"
        "var d=new TextDecoder();assert(d.encoding==='utf-8'&&d.fatal===false&&d.ignoreBOM===false);"
        "assert(new TextDecoder(' latin1 ').encoding==='windows-1252'&&new TextDecoder('ASCII').encoding==='windows-1252');"
        "assert(new TextDecoder('utf-16').encoding==='utf-16le'&&new TextDecoder('unicode').encoding==='utf-16le');"
        "assert(new TextDecoder('utf-16be').encoding==='utf-16be'&&new TextDecoder('utf8').encoding==='utf-8');"
        "assert(new TextDecoder(undefined).encoding==='utf-8');"
        "for(const no of ['shift_jis','replacement','iso-2022-kr','','utf-32','x'])throws('RangeError',()=>new TextDecoder(no));"
        "var o=new TextDecoder('utf-8',{fatal:1,ignoreBOM:'yes'});assert(o.fatal===true&&o.ignoreBOM===true);"
        "throws('TypeError',()=>new TextDecoder('utf-8',5));throws('TypeError',()=>TextDecoder());"
        "throws('TypeError',()=>TextDecoder.prototype.decode.call({}));"
        "assert(d.decode()===''&&d.decode(undefined)===''&&d.decode(u8([0x61,0xe2,0x82,0xac]))==='a\\u20ac');"
        "assert(d.decode(u8([0x61,0x62]).buffer)==='ab','an ArrayBuffer');"
        "var buf=u8([0x78,0x41,0x42,0x79]).buffer;"
        "assert(d.decode(new Uint8Array(buf,1,2))==='AB'&&d.decode(new DataView(buf,1,2))==='AB','views at an offset');"
        "assert(d.decode(new Uint16Array([0x6261]))==='ab','any typed array is bytes');"
        "throws('TypeError',()=>d.decode('ab'));throws('TypeError',()=>d.decode([97]));throws('TypeError',()=>d.decode({}));"
        "throws('TypeError',()=>d.decode(u8([97]),5));"
        /* Errors: U+FFFD per maximal subpart, the byte that did not fit read
         * again. */
        "assert(d.decode(u8([0xc0,0x41]))==='\\ufffdA'&&d.decode(u8([0xe2,0x82,0x41]))==='\\ufffdA');"
        "assert(d.decode(u8([0xed,0xa0,0x80]))==='\\ufffd\\ufffd\\ufffd','an encoded surrogate is three errors');"
        "assert(d.decode(u8([0xf0,0x90,0x80]))==='\\ufffd','a truncated sequence at the end is one');"
        "assert(d.decode(u8([0xff,0xfe,0x00]))==='\\ufffd\\ufffd\\u0000');"
        /* The BOM: dropped once at the start, kept with ignoreBOM. */
        "assert(d.decode(u8([0xef,0xbb,0xbf,0x41,0xef,0xbb,0xbf]))==='A\\ufeff');"
        "assert(new TextDecoder('utf-8',{ignoreBOM:true}).decode(u8([0xef,0xbb,0xbf,0x41]))==='\\ufeffA');"
        "assert(new TextDecoder('utf-16le').decode(u8([0xff,0xfe,0x41,0,0x3d,0xd8,0x00,0xde]))==='A\\ud83d\\ude00');"
        "assert(new TextDecoder('utf-16be').decode(u8([0xfe,0xff,0,0x41,0xd8,0x3d]))==='A\\ufffd');"
        "assert(new TextDecoder('utf-16le').decode(u8([0x00,0xdc,0x41,0x00]))==='\\ufffdA','a lone trail');"
        "assert(new TextDecoder('utf-16le').decode(u8([0x3d,0xd8,0x41,0x00]))==='\\ufffdA','a lead with no trail, the unit read again');"
        "assert(new TextDecoder('utf-16le').decode(u8([0x41]))==='\\ufffd','an odd byte at the end');"
        "assert(new TextDecoder('windows-1252').decode(u8([0x80,0x81,0x9f,0xe9]))==='\\u20ac\\u0081\\u0178\\u00e9');"
        "assert(new TextDecoder('windows-1252').decode(u8([0xef,0xbb,0xbf]))==='\\u00ef\\u00bb\\u00bf','no BOM in windows-1252');",
        "encoding: TextDecoder's labels, inputs, errors and BOM");
    dom_script(&f, STORAGE_ASSERT
        "function u8(a){return new Uint8Array(a)}"
        /* Streaming: a sequence split between calls, the BOM's rule across
         * them, and a flush without input. */
        "var s=new TextDecoder();assert(s.decode(u8([0xe2,0x82]),{stream:true})==='');"
        "assert(s.decode(u8([0xac,0x41]),{stream:true})==='\\u20acA'&&s.decode()==='');"
        "assert(s.decode(u8([0xc9]),{stream:true})===''&&s.decode(undefined)==='\\ufffd');"
        "assert(s.decode(u8([0xef,0xbb]),{stream:true})===''&&s.decode(u8([0xbf,0x41]),{stream:true})==='A');"
        "assert(s.decode(u8([0xef,0xbb,0xbf]))==='\\ufeff','one BOM a stream');"
        "assert(s.decode(u8([0xef,0xbb,0xbf]))==='','a new decode starts a new stream');"
        "var w=new TextDecoder('utf-16le');assert(w.decode(u8([0x41]),{stream:true})===''&&w.decode(u8([0]))==='A');"
        "assert(w.decode(u8([0x3d,0xd8]),{stream:true})===''&&w.decode(u8([0x00,0xde]))==='\\ud83d\\ude00');"
        "var opts={get stream(){return true}};assert(s.decode(u8([0xe2]),opts)===''&&s.decode(u8([0x82,0xac]))==='\\u20ac');"
        /* Fatal: the decode throws, and the state is where the error left it. */
        "var f=new TextDecoder('utf-8',{fatal:true});throws('TypeError',()=>f.decode(u8([0xff])));"
        "assert(f.decode(u8([0x41]))==='A');throws('TypeError',()=>f.decode(u8([0xc2])));"
        "assert(f.decode(u8([0xc2]),{stream:true})==='');throws('TypeError',()=>f.decode(u8([0x41])));"
        "var g=new TextDecoder('utf-16le',{fatal:true});"
        "assert(g.decode(u8([0]),{stream:true})===''&&g.decode(u8([0]))==='\\u0000');"
        "throws('TypeError',()=>{g.decode(u8([0,0]),{stream:true});g.decode(u8([0]))});"
        "assert(g.decode(u8([0,0]),{stream:true})==='\\u0000'&&g.decode(u8([0,0]))==='\\u0000');"
        /* A buffer detached while the options are read decodes as nothing. */
        "var arr=new Uint8Array(100).fill(65);"
        "assert(new TextDecoder().decode(arr,{get stream(){arr.buffer.transfer(0);return false}})==='');"
        "var view=new DataView(new ArrayBuffer(8));"
        "assert(new TextDecoder().decode(view,{get stream(){view.buffer.transfer(0);return false}})==='');",
        "encoding: TextDecoder's streaming, fatal and detached buffers");
    fixture_free(&f);
    static const char *const kOperations[] = {
        "new TextEncoder().encode('a\\u20ac')", "new TextEncoder().encodeInto('ab',new Uint8Array(4))",
        "new TextDecoder('latin1')", "new TextDecoder().decode(new Uint8Array([0x61,0xe2,0x82,0xac]))",
        "new TextDecoder().decode(new DataView(new ArrayBuffer(2)))", "btoa('foo')", "atob('Zm9v')",
    };
    binding_sweep("text", kOperations, sizeof(kOperations) / sizeof(kOperations[0]), NULL);
    engine_sweep("text", kOperations, sizeof(kOperations) / sizeof(kOperations[0]), NULL);
}
