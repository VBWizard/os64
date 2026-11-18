/* FILEREADER (libdom/file_reader.c; BLOB_URLS.md decision 10): each read
 * kind's result; the one queued task, its source named, and its events in
 * order (loadstart, progress, load, loadend) with their loaded and total;
 * readAsText's encoding (the label, else the Blob's charset, else UTF-8,
 * a BOM over all three); the InvalidStateError of a read while LOADING;
 * abort() while LOADING, inside a handler, and in EMPTY or DONE; the
 * chunked-reading loop, whose load handler suppresses the first loadend;
 * a reader dropped by the page with its read queued; and a page torn down
 * with one queued (the fixture's teardown counts every byte). */

/* The reader's next task, as the host runs it from the message queue. */
static bool reader_task(DomFixture *f)
{
    os64_js_outcome_t outcome;
    bool ran = os64_dom_message_fire(f->binding, &outcome);
    check(!ran || outcome.status == OS64_JS_OK, "file reader: the task ran clean");
    return ran;
}

#define READER_LOG "globalThis.log=[];function watch(r){for(const t of ['loadstart','progress','load','abort','error','loadend'])" \
    "r.addEventListener(t,e=>log.push(t+':'+e.loaded+'/'+e.total+':'+r.readyState));}"

static void dom_file_reader_cases(void)
{
    DomFixture f = blob_fixture("https://example.test/", NULL, NULL);
    check(f.binding != NULL, "file reader: the binding is made");
    dom_script(&f, STORAGE_ASSERT READER_LOG
        "assert(FileReader.EMPTY===0&&FileReader.LOADING===1&&FileReader.DONE===2,'the constants');"
        "const r=new FileReader();globalThis.r=r;watch(r);"
        "assert(r.readyState===0&&r.result===null&&r.error===null,'a new reader is empty');"
        "assert(r instanceof FileReader&&typeof r.addEventListener==='function','an event target');"
        "throws('TypeError',()=>r.readAsText({}));"
        "r.readAsText(new Blob(['h\\u00e9llo']));"
        "assert(r.readyState===1&&r.result===null&&log.length===0,'LOADING, and nothing fired yet');"
        "throws('InvalidStateError',()=>r.readAsArrayBuffer(new Blob([])));",
        "file reader: a read begun");
    check(reader_task(&f), "file reader: one task");
    const char *kind = os64_dom_message_kind(f.binding);
    check(kind != NULL && !strcmp(kind, "file reading"), "file reader: the task that ran is named by its source");
    check(!os64_dom_message_fire(f.binding, &(os64_js_outcome_t){0}), "file reader: and only one");
    dom_script(&f, STORAGE_ASSERT
        "assert(log.join()==='loadstart:0/6:1,progress:6/6:1,load:6/6:2,loadend:6/6:2',log.join());"
        "assert(r.readyState===2&&r.result==='h\\u00e9llo',String(r.result));",
        "file reader: the events in order, and the text");

    /* Each kind's result. */
    dom_script(&f, STORAGE_ASSERT
        "log.length=0;r.readAsArrayBuffer(new Blob([new Uint8Array([1,2,255])]));"
        "assert(r.result===null,'a new read clears the result');",
        "file reader: an ArrayBuffer read");
    reader_task(&f);
    dom_script(&f, STORAGE_ASSERT
        "assert(r.result instanceof ArrayBuffer&&new Uint8Array(r.result).join()==='1,2,255',String(r.result));"
        "r.readAsBinaryString(new Blob([new Uint8Array([0,65,128,255])]));",
        "file reader: the ArrayBuffer");
    reader_task(&f);
    dom_script(&f, STORAGE_ASSERT
        "const s=r.result;assert(s.length===4&&[...s].map(c=>c.charCodeAt(0)).join()==='0,65,128,255',escape(s));"
        "r.readAsDataURL(new Blob(['hi'],{type:'text/plain'}));",
        "file reader: the binary string");
    reader_task(&f);
    dom_script(&f, STORAGE_ASSERT
        "assert(r.result==='data:text/plain;base64,aGk=',r.result);r.readAsDataURL(new Blob(['abc']));",
        "file reader: the data: URL");
    reader_task(&f);
    dom_script(&f, STORAGE_ASSERT
        "assert(r.result==='data:application/octet-stream;base64,YWJj',r.result);"
        "log.length=0;r.readAsText(new Blob([]));",
        "file reader: an untyped Blob's data: URL");
    reader_task(&f);
    dom_script(&f, STORAGE_ASSERT
        "assert(log.join()==='loadstart:0/0:1,load:0/0:2,loadend:0/0:2'&&r.result==='',log.join());",
        "file reader: an empty Blob has no progress");

    /* readAsText's encoding: the label, the Blob's charset, UTF-8, and a BOM
     * over all three. */
    static const struct { const char *start, *want, *name; } texts[] = {
        {"r.readAsText(new Blob([new Uint8Array([0x68,0,0x69,0])]),'utf-16le');", "hi", "file reader: the label"},
        {"r.readAsText(new Blob([new Uint8Array([0xe9])],{type:'text/plain;charset=latin1'}));", "\\u00e9",
         "file reader: the Blob's charset"},
        {"r.readAsText(new Blob([new Uint8Array([0xe9])],{type:'text/plain;charset=latin1'}),'nonsense');", "\\u00e9",
         "file reader: an unknown label falls to the charset"},
        {"r.readAsText(new Blob([new Uint8Array([0xff,0xfe,0x68,0])]),'utf-8');", "h",
         "file reader: a BOM outranks the label"},
        {"r.readAsText(new Blob([new Uint8Array([0xef,0xbb,0xbf,0x6f,0x6b])]));", "ok",
         "file reader: UTF-8's BOM is taken off"},
    };
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
        dom_script(&f, texts[i].start, texts[i].name);
        reader_task(&f);
        char source[256];
        snprintf(source, sizeof(source), STORAGE_ASSERT "assert(r.result==='%s',escape(r.result));", texts[i].want);
        dom_script(&f, source, texts[i].name);
    }

    /* abort(): while LOADING it fires abort and loadend at once, and the
     * queued task then does nothing; in EMPTY or DONE it fires nothing and
     * clears the result. */
    dom_script(&f, STORAGE_ASSERT
        "log.length=0;r.readAsText(new Blob(['x']));r.abort();"
        "assert(log.join()==='abort:0/0:2,loadend:0/0:2',log.join());"
        "assert(r.readyState===2&&r.result===null,'DONE with no result');",
        "file reader: abort while LOADING");
    check(reader_task(&f), "file reader: the aborted read's task is still the queue's");
    dom_script(&f, STORAGE_ASSERT
        "assert(log.length===2,'and does nothing: '+log.join());"
        "r.readAsText(new Blob(['y']));",
        "file reader: the aborted read's task fired nothing");
    reader_task(&f);
    dom_script(&f, STORAGE_ASSERT
        "log.length=0;assert(r.result==='y');r.abort();"
        "assert(log.length===0&&r.result===null&&r.readyState===2,'abort in DONE: '+log.join());"
        "const e=new FileReader();watch(e);e.abort();assert(log.length===0&&e.readyState===0,'abort in EMPTY');",
        "file reader: abort in DONE and EMPTY");

    /* abort() from the loadstart handler: no progress, no load. */
    dom_script(&f, STORAGE_ASSERT
        "log.length=0;const a=new FileReader();watch(a);a.onloadstart=()=>a.abort();a.readAsText(new Blob(['z']));",
        "file reader: an abort in loadstart");
    reader_task(&f);
    dom_script(&f, STORAGE_ASSERT
        "assert(log.join()==='loadstart:0/1:1,abort:0/0:2,loadend:0/0:2',log.join());",
        "file reader: loadstart, then the abort's two");

    /* The chunked-reading loop: a load handler that starts the next read
     * suppresses the first read's loadend, and the next read runs in a
     * task of its own. */
    dom_script(&f, STORAGE_ASSERT
        "log.length=0;const c=new FileReader();watch(c);let n=0;"
        "c.onload=()=>{if(++n<2)c.readAsText(new Blob(['ab']));};c.readAsText(new Blob(['a']));",
        "file reader: the chunked loop");
    reader_task(&f);
    dom_script(&f, STORAGE_ASSERT
        "assert(log.join()==='loadstart:0/1:1,progress:1/1:1,load:1/1:2',log.join());",
        "file reader: the first read's loadend suppressed");
    reader_task(&f);
    dom_script(&f, STORAGE_ASSERT
        "assert(log.slice(3).join()==='loadstart:0/2:1,progress:2/2:1,load:2/2:2,loadend:2/2:2',log.join());",
        "file reader: the next read whole");

    /* A reader the page drops with its read queued: the task holds it and
     * still delivers. */
    dom_script(&f, "globalThis.late='';(()=>{const d=new FileReader();d.onload=()=>{late=d.result;};"
                   "d.readAsText(new Blob(['kept']));})();",
               "file reader: a reader dropped with its read queued");
    os64_js_outcome_t gc;
    JS_RunGC(JS_GetRuntime(os64_js_context(f.runtime, OS64_JS_ABI_ID, &gc)));
    reader_task(&f);
    dom_script(&f, STORAGE_ASSERT "assert(late==='kept',late);", "file reader: its read still answers");

    /* Torn down with a read queued: the queue drops it, and nothing leaks. */
    dom_script(&f, "new FileReader().readAsArrayBuffer(new Blob(['left']));", "file reader: a read left queued");
    fixture_free(&f);
}
