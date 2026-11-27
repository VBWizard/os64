/* THE FULLSCREEN API (libdom/fullscreen.c; VIDEO.md § 9, the design
 * state's Part B): the members and their prefixed names, a request with
 * and without the person's activation, the host's answers (in, refused,
 * out), a request for an element in no document, exitFullscreen, the
 * element removed while full screen, a request for another element, the
 * binding drained under a full-screen element, and a document with no
 * host. A fake host records what it was asked; the view step fires the
 * events, one a call. The page records each event it hears in L as
 * "type@where". */

#define FS_ASSERT STORAGE_ASSERT \
    "globalThis.L=globalThis.L||[];" \
    "function hear(t){for(const k of ['fullscreenchange','fullscreenerror','webkitfullscreenchange'," \
    "'webkitfullscreenerror'])t.addEventListener(k,e=>L.push(k+'@'+(e.currentTarget===document?'doc':" \
    "e.currentTarget.id)));}" \
    "function heard(){const s=L.join(' ');L.length=0;return s}"

static struct {
    bool activated;
    unsigned entered, left;
    const os64_html_node_t *asked;
} fs_host;

static bool fs_consume(void *opaque)
{
    (void)opaque;
    bool was = fs_host.activated;
    fs_host.activated = false;
    return was;
}
static void fs_enter(void *opaque, const os64_html_node_t *element)
{
    (void)opaque;
    fs_host.entered++;
    fs_host.asked = element;
}
static void fs_leave(void *opaque) { (void)opaque; fs_host.left++; }

static const os64_dom_fullscreen_provider_t fs_provider = {NULL, fs_consume, fs_enter, fs_leave};

static ViewFake fs_view;

static DomFixture fs_fixture(bool with_host)
{
    memset(&fs_host, 0, sizeof(fs_host));
    memset(&fs_view, 0, sizeof(fs_view));
    fs_view.view.width = 800;
    fs_view.view.height = 600;
    os64_dom_options_t options = p2b_options(P2B_URL);
    DomFixture f = fixture_new(&options);
    if (fixture_ready(&f)) {
        os64_dom_view_provider_t view = {fake_view_read, fake_view_scroll, &fs_view, NULL};
        os64_dom_set_view(f.binding, &view);
        if (with_host)
            os64_dom_set_fullscreen(f.binding, &fs_provider);
    }
    return f;
}

static int fs_script(DomFixture *f, const char *body, const char *name)
{
    size_t length = strlen(FS_ASSERT) + strlen(body) + 3;
    char *source = malloc(length);
    if (source == NULL) abort();
    snprintf(source, length, "%s{%s}", FS_ASSERT, body);
    int result = dom_script(f, source, name);
    free(source);
    return result;
}

/* View steps until none runs, each OK. */
static void fs_steps(DomFixture *f, const char *what)
{
    os64_js_outcome_t out;
    for (int i = 0; i < 32 && os64_dom_view_step(f->binding, &out); i++) {
        if (out.status != OS64_JS_OK) fprintf(stderr, "%s: %s\n", what, out.message);
        check(out.status == OS64_JS_OK, what);
    }
}

static void dom_fullscreen_cases(void)
{
    DomFixture f = fs_fixture(true);
    if (!fixture_ready(&f)) return;
    fs_script(&f, "globalThis.v=document.createElement('div');v.id='v';document.body.appendChild(v);"
                  "globalThis.w=document.createElement('div');w.id='w';document.body.appendChild(w);"
                  "hear(document);hear(v);hear(w);"
                  "for(const n of ['requestFullscreen','webkitRequestFullscreen','webkitRequestFullScreen'])"
                  "assert(typeof Element.prototype[n]==='function','Element has '+n);"
                  "for(const n of ['exitFullscreen','webkitExitFullscreen','webkitCancelFullScreen'])"
                  "assert(typeof document[n]==='function','Document has '+n);"
                  "assert(document.fullscreenEnabled===true&&document.webkitFullscreenEnabled===true,'enabled');"
                  "assert(document.fullscreenElement===null&&document.fullscreen===false&&"
                  "document.webkitIsFullScreen===false,'nothing is full screen');"
                  "assert('onfullscreenchange' in document&&'onfullscreenerror' in v,'the handlers');",
              "fullscreen: the members");

    /* No activation: refused, with fullscreenerror, and the host not asked. */
    fs_script(&f, "globalThis.R=[];v.requestFullscreen().then(()=>R.push('ok'),e=>R.push(e.name));",
              "fullscreen: a request without activation");
    check(fs_host.entered == 0, "fullscreen: the host is not asked without activation");
    fs_steps(&f, "fullscreen: the refusal's task");
    fs_script(&f, "const h=heard();assert(h==='fullscreenerror@v fullscreenerror@doc webkitfullscreenerror@v "
                  "webkitfullscreenerror@doc','the error, bubbling, then the prefixed one: '+h);"
                  "assert(R.join()==='TypeError','rejected TypeError: '+R.join());",
              "fullscreen: refused without activation");

    /* An element in no document is refused before the activation is asked
     * for, which is left for the next request. */
    fs_host.activated = true;
    fs_script(&f, "R.length=0;document.createElement('div').requestFullscreen().then(()=>R.push('ok'),"
                  "e=>R.push(e.name));",
              "fullscreen: a request for an element in no document");
    check(fs_host.entered == 0 && fs_host.activated, "fullscreen: a detached element is refused, the host not asked");
    fs_steps(&f, "fullscreen: the detached refusal's task");
    fs_script(&f, "heard();assert(R.join()==='TypeError','detached: '+R.join());R.length=0;",
              "fullscreen: refused for a detached element");
    fs_host.activated = false;

    /* With activation: asked of the host; the change after its answer. */
    fs_host.activated = true;
    fs_script(&f, "R.length=0;v.requestFullscreen().then(()=>R.push('ok:'+L.join()),e=>R.push(e.name));",
              "fullscreen: a request with activation");
    check(fs_host.entered == 1 && !fs_host.activated, "fullscreen: the host is asked, the activation used up");
    fs_steps(&f, "fullscreen: nothing before the answer");
    /* The second request carries an activation of its own, so only the
     * change under way can refuse it, and the activation is left. */
    fs_host.activated = true;
    fs_script(&f, "assert(R.length===0&&document.fullscreenElement===null,'nothing before the host answers');"
                  "v.requestFullscreen().then(()=>R.push('again'),e=>R.push('again:'+e.name));",
              "fullscreen: a second request while entering");
    check(fs_host.entered == 1 && fs_host.activated, "fullscreen: the second request is refused, the host not asked");
    fs_host.activated = false;
    fs_steps(&f, "fullscreen: the second request's refusal");
    fs_script(&f, "const h=heard();assert(R.join()==='again:TypeError'&&h==='fullscreenerror@v fullscreenerror@doc "
                  "webkitfullscreenerror@v webkitfullscreenerror@doc','refused while entering: '+R.join()+' '+h);"
                  "R.length=0;",
              "fullscreen: refused while entering");
    os64_dom_fullscreen_settled(f.binding, true);
    fs_steps(&f, "fullscreen: entered");
    fs_script(&f, "assert(R.join()==='ok:fullscreenchange@v,fullscreenchange@doc,"
                  "webkitfullscreenchange@v,webkitfullscreenchange@doc','the change before the promise: '+R.join());"
                  "heard();"
                  "assert(document.fullscreenElement===v&&document.webkitFullscreenElement===v&&"
                  "document.webkitCurrentFullScreenElement===v&&document.fullscreen&&document.webkitIsFullScreen,"
                  "'v is the fullscreen element');"
                  "assert(v.matches(':fullscreen')&&v.matches(':-webkit-full-screen')&&!w.matches(':fullscreen'),"
                  "':fullscreen matches v alone');",
              "fullscreen: v is full screen");
    check(os64_dom_fullscreen_element(f.binding) == fs_host.asked, "fullscreen: the host reads the element");

    /* The same element again resolves with no change. */
    fs_script(&f, "R.length=0;v.requestFullscreen().then(()=>R.push('same'));", "fullscreen: the same again");
    fs_steps(&f, "fullscreen: the same again");
    fs_script(&f, "assert(R.join()==='same'&&heard()==='','resolved, no event: '+R.join());",
              "fullscreen: the same element");
    check(fs_host.entered == 1, "fullscreen: the host is not asked for the same element");

    /* Another element replaces it: the old one's change, then the new. */
    fs_host.activated = true;
    fs_script(&f, "R.length=0;w.requestFullscreen().then(()=>R.push('w'));", "fullscreen: another element");
    os64_dom_fullscreen_settled(f.binding, true);
    fs_steps(&f, "fullscreen: replaced");
    fs_script(&f, "const h=heard();assert(h==='fullscreenchange@v fullscreenchange@doc webkitfullscreenchange@v "
                  "webkitfullscreenchange@doc fullscreenchange@w fullscreenchange@doc webkitfullscreenchange@w "
                  "webkitfullscreenchange@doc','v leaves, then w: '+h);"
                  "assert(R.join()==='w'&&document.fullscreenElement===w&&!v.matches(':fullscreen'),'w now');",
              "fullscreen: w replaces v");

    /* exitFullscreen: asked of the host; the change after its answer. */
    fs_script(&f, "R.length=0;document.exitFullscreen().then(()=>R.push('out:'+L.join()));",
              "fullscreen: exitFullscreen");
    check(fs_host.left == 1, "fullscreen: the host is asked to leave");
    os64_dom_fullscreen_exit(f.binding);
    fs_steps(&f, "fullscreen: left");
    fs_script(&f, "assert(R.join()==='out:fullscreenchange@w,fullscreenchange@doc,webkitfullscreenchange@w,"
                  "webkitfullscreenchange@doc','the change, then the promise: '+R.join());heard();"
                  "assert(document.fullscreenElement===null&&!w.matches(':fullscreen'),'nothing full screen');"
                  "document.exitFullscreen().then(()=>R.push('none'),e=>R.push(e.name));",
              "fullscreen: out");
    fs_steps(&f, "fullscreen: exit with nothing full screen");
    fs_script(&f, "assert(R.pop()==='TypeError','exit with none: TypeError');", "fullscreen: exit with none");

    /* The host refuses. */
    fs_host.activated = true;
    fs_script(&f, "R.length=0;v.requestFullscreen().then(()=>R.push('ok'),e=>R.push(e.name));",
              "fullscreen: a request the host refuses");
    os64_dom_fullscreen_settled(f.binding, false);
    fs_steps(&f, "fullscreen: refused");
    fs_script(&f, "const h=heard();assert(h==='fullscreenerror@v fullscreenerror@doc webkitfullscreenerror@v "
                  "webkitfullscreenerror@doc'&&R.join()==='TypeError','the host refused: '+h+' '+R.join());"
                  "assert(document.fullscreenElement===null,'still nothing');",
              "fullscreen: refused by the host");

    /* The element removed while full screen: the host is asked to leave,
     * and the change goes to the document. */
    fs_host.activated = true;
    fs_script(&f, "v.requestFullscreen();", "fullscreen: v again");
    os64_dom_fullscreen_settled(f.binding, true);
    fs_steps(&f, "fullscreen: v again");
    fs_script(&f, "heard();v.remove();", "fullscreen: v removed");
    unsigned before = fs_host.left;
    fs_steps(&f, "fullscreen: the removal seen");
    check(fs_host.left == before + 1, "fullscreen: a removed element asks the host to leave");
    os64_dom_fullscreen_exit(f.binding);
    fs_steps(&f, "fullscreen: left after removal");
    fs_script(&f, "const h=heard();assert(h==='fullscreenchange@doc webkitfullscreenchange@doc',"
                  "'the change at the document: '+h);"
                  "assert(document.fullscreenElement===null&&!v.matches(':fullscreen'),'cleared');"
                  "document.body.appendChild(v);",
              "fullscreen: removed");

    /* The host's own exit (Escape, the window manager's chord). */
    fs_host.activated = true;
    fs_script(&f, "v.webkitRequestFullscreen();", "fullscreen: prefixed request");
    check(fs_host.entered == 5, "fullscreen: the prefixed request asks the host");
    os64_dom_fullscreen_settled(f.binding, true);
    fs_steps(&f, "fullscreen: prefixed in");
    fs_script(&f, "heard();assert(document.webkitFullscreenElement===v&&v.webkitRequestFullScreen()===undefined,"
                  "'in, and the prefixed request answers nothing');",
              "fullscreen: prefixed");
    os64_dom_fullscreen_exit(f.binding);
    fs_steps(&f, "fullscreen: the host's exit");
    fs_script(&f, "const h=heard();assert(h==='fullscreenchange@v fullscreenchange@doc webkitfullscreenchange@v "
                  "webkitfullscreenchange@doc'&&document.fullscreenElement===null,'the host left: '+h);",
              "fullscreen: the host's exit");

    /* The binding drained under a full-screen element, its document kept (a
     * retired host's page stays on screen): the element's bit is cleared,
     * and nothing is full screen any more. */
    fs_host.activated = true;
    fs_script(&f, "v.requestFullscreen();", "fullscreen: in before the drain");
    os64_dom_fullscreen_settled(f.binding, true);
    fs_steps(&f, "fullscreen: in before the drain, settled");
    const os64_html_node_t *held = fs_host.asked;
    check(held != NULL && (held->state & OS64_HTML_STATE_FULLSCREEN) != 0 &&
          os64_dom_fullscreen_element(f.binding) == held, "fullscreen: in before the drain");
    os64_dom_drain(f.binding);
    check((held->state & OS64_HTML_STATE_FULLSCREEN) == 0 && os64_dom_fullscreen_element(f.binding) == NULL,
          "fullscreen: the drain clears the element's state and names none");
    fixture_free(&f);

    /* A document with no host: not enabled, and refused. */
    DomFixture g = fs_fixture(false);
    if (!fixture_ready(&g)) return;
    fs_script(&g, "assert(document.fullscreenEnabled===false,'not enabled');globalThis.R=[];"
                  "document.body.requestFullscreen().then(()=>R.push('ok'),e=>R.push(e.name+': '+e.message));",
              "fullscreen: no host");
    fs_steps(&g, "fullscreen: no host's refusal");
    fs_script(&g, "assert(R.join()==='TypeError: Fullscreen is not enabled in this document','refused: '+R.join());",
              "fullscreen: refused with no host");
    fixture_free(&g);
}
