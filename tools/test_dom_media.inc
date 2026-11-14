/* THE MEDIA ELEMENT (libdom/media.c; AUDIO_YONDER.md § The element in
 * libdom, § The element's states): the interfaces, the states and the
 * events each of the standard's steps fires, play()'s promises, autoplay's
 * rule, the busy wait, and the sessions it opens and closes, against a
 * fake host whose facts the case sets by hand before each media turn.
 * The page records each event it hears in L as "type:readyState". */

#define MEDIA_ASSERT STORAGE_ASSERT \
    "globalThis.L=globalThis.L||[];" \
    "function hear(a){for(const t of ['loadstart','progress','suspend','abort','error','emptied','stalled'," \
    "'loadedmetadata','loadeddata','canplay','canplaythrough','playing','waiting','seeking','seeked','ended'," \
    "'durationchange','timeupdate','play','pause','ratechange','volumechange'])" \
    "a.addEventListener(t,()=>L.push(t+':'+a.readyState));}" \
    "function heard(){const s=L.join(' ');L.length=0;return s}"

typedef struct {
    bool open;
    char url[160];
    os64_dom_media_cors_t cors;
    os64_dom_media_preload_t preload;
    bool playing;
    double gain;
    bool loop;
    double seek_to;
    uint64_t seek_generation;
    os64_dom_media_facts_t facts;
} MediaSession;

static MediaSession media_sessions[8];
static size_t media_opened, media_closed;
static bool media_activated, media_refuse;

static void *media_open(void *opaque, const os64_html_node_t *node, const char *url, os64_dom_media_cors_t cors,
                        os64_dom_media_preload_t preload)
{
    (void)opaque; (void)node;
    if (media_refuse || media_opened == sizeof(media_sessions) / sizeof(media_sessions[0])) return NULL;
    MediaSession *s = &media_sessions[media_opened++];
    memset(s, 0, sizeof(*s));
    s->open = true;
    snprintf(s->url, sizeof(s->url), "%s", url);
    s->cors = cors;
    s->preload = preload;
    s->gain = -1;
    return s;
}
static void media_close(void *opaque, void *session) { (void)opaque; ((MediaSession *)session)->open = false; media_closed++; }
static void media_play(void *opaque, void *session) { (void)opaque; ((MediaSession *)session)->playing = true; }
static void media_pause(void *opaque, void *session) { (void)opaque; ((MediaSession *)session)->playing = false; }
static void media_seek(void *opaque, void *session, double t, uint64_t generation)
{
    (void)opaque;
    MediaSession *s = session;
    s->seek_to = t;
    s->seek_generation = generation;
}
static void media_settings(void *opaque, void *session, double gain, bool loop)
{
    (void)opaque;
    ((MediaSession *)session)->gain = gain;
    ((MediaSession *)session)->loop = loop;
}
static void media_facts(void *opaque, void *session, os64_dom_media_facts_t *out)
{
    (void)opaque;
    *out = ((MediaSession *)session)->facts;
}
static bool media_is_activated(void *opaque) { (void)opaque; return media_activated; }
static int media_can_play(void *opaque, const char *type)
{
    (void)opaque;
    if (strncmp(type, "audio/mpeg", 10) == 0) return strstr(type, "codecs") != NULL ? 2 : 1;
    return 0;
}

static const os64_dom_media_provider_t media_provider = {
    NULL, media_open, media_close, media_play, media_pause, media_seek, media_settings, media_facts,
    media_is_activated, media_can_play,
};

static DomFixture media_fixture(DomFixture *slot)
{
    memset(media_sessions, 0, sizeof(media_sessions));
    media_opened = media_closed = 0;
    media_activated = media_refuse = false;
    p2b_ms = 1000;
    p2b_us = 1000000;
    os64_dom_options_t options = p2b_options(P2B_URL);
    *slot = fixture_new(&options);
    if (fixture_ready(slot)) os64_dom_set_media(slot->binding, &media_provider);
    return *slot;
}

static int media_script(DomFixture *f, const char *body, const char *name)
{
    size_t length = strlen(MEDIA_ASSERT) + strlen(body) + 3;
    char *source = malloc(length);
    if (source == NULL) abort();
    snprintf(source, length, "%s{%s}", MEDIA_ASSERT, body);
    int result = dom_script(f, source, name);
    free(source);
    return result;
}

/* A media turn, and every task it queued run, each one OK. */
static void media_turn(DomFixture *f, const char *what)
{
    os64_dom_media_turn(f->binding);
    os64_js_outcome_t out;
    for (int i = 0; i < 64 && os64_dom_media_fire(f->binding, &out); i++) {
        if (out.status != OS64_JS_OK) fprintf(stderr, "%s: %s\n", what, out.message);
        check(out.status == OS64_JS_OK, what);
    }
}

/* The session a probe would leave: its length known, sound decoded. */
static void media_ready(MediaSession *s, double duration)
{
    s->facts.probed = true;
    s->facts.duration = duration;
    s->facts.seekable = true;
    s->facts.current = s->facts.future = s->facts.enough = true;
    s->facts.arrived = duration;
}

static void dom_media_interface_cases(void)
{
    DomFixture f;
    media_fixture(&f);
    if (!fixture_ready(&f)) return;
    media_script(&f,
        "assert(Object.getPrototypeOf(HTMLAudioElement)===HTMLMediaElement&&"
        "Object.getPrototypeOf(HTMLMediaElement)===HTMLElement,'the interfaces inherit');"
        "const a=document.createElement('audio');"
        "assert(a instanceof HTMLAudioElement&&a instanceof HTMLMediaElement,'createElement makes one');"
        "assert(String(a)==='[object HTMLAudioElement]');"
        "assert(Audio.prototype===HTMLAudioElement.prototype&&HTMLAudioElement.prototype.constructor===HTMLAudioElement);"
        "const b=new Audio();assert(b instanceof HTMLAudioElement&&b.preload==='auto'&&b.src==='','new Audio()');"
        "throws('TypeError',()=>new HTMLMediaElement());throws('TypeError',()=>new MediaError());"
        "throws('TypeError',()=>new TimeRanges());"
        "assert(HTMLMediaElement.HAVE_ENOUGH_DATA===4&&a.NETWORK_NO_SOURCE===3&&MediaError.MEDIA_ERR_SRC_NOT_SUPPORTED===4);"
        "assert(a.networkState===0&&a.readyState===0&&a.paused&&!a.seeking&&!a.ended&&a.error===null,'the start');"
        "assert(isNaN(a.duration)&&a.currentTime===0&&a.volume===1&&!a.muted&&a.playbackRate===1,'the numbers');"
        "assert(a.buffered.length===0&&a.played.length===0&&a.seekable.length===0,'no ranges');"
        "throws('IndexSizeError',()=>a.buffered.start(0));"
        "assert(a.preload==='metadata','preload defaults to metadata');a.preload='';assert(a.preload==='auto');"
        "a.preload='bogus';assert(a.preload==='metadata');a.preload='NONE';assert(a.preload==='none');"
        "assert(a.crossOrigin===null);a.crossOrigin='x';assert(a.crossOrigin==='anonymous');"
        "a.crossOrigin='USE-credentials';assert(a.crossOrigin==='use-credentials');a.crossOrigin=null;"
        "assert(!a.hasAttribute('crossorigin'));"
        "a.loop=true;a.autoplay=true;a.controls=true;a.defaultMuted=true;"
        "assert(a.getAttribute('loop')===''&&a.hasAttribute('autoplay')&&a.hasAttribute('controls')&&"
        "a.hasAttribute('muted')&&!a.muted,'reflections; defaultMuted is not muted');"
        "assert(a.canPlayType('audio/mpeg')==='maybe'&&a.canPlayType('audio/mpeg; codecs=mp3')==='probably'&&"
        "a.canPlayType('video/webm')==='','canPlayType is the host\\'s');"
        "throws('IndexSizeError',()=>{a.volume=1.5});throws('TypeError',()=>{a.currentTime=NaN});"
        "throws('TypeError',()=>HTMLMediaElement.prototype.play.call(document.body));"
        "for(const h of ['onplay','onended','ontimeupdate','onvolumechange'])"
        "assert(h in window&&h in document&&h in a,h);"
        "assert('onerror' in a&&!('onerror' in document.createElement('div')),'a media element has onerror');",
        "media: the interfaces");
    fixture_free(&f);
}

static void dom_media_play_cases(void)
{
    DomFixture f;
    media_fixture(&f);
    if (!fixture_ready(&f)) return;
    /* new Audio(src): a load at once, and the session at the next turn. */
    media_script(&f, "globalThis.a=new Audio('song.mp3');hear(a);"
                     "assert(a.networkState===3&&a.readyState===0,'the load algorithm ran: '+a.networkState);",
                 "media: new Audio(src)");
    check(media_opened == 0, "media: no session before the turn");
    media_turn(&f, "media: the turn opens it");
    MediaSession *s = &media_sessions[0];
    check(media_opened == 1 && s->open && strcmp(s->url, "https://example.test/dir/song.mp3") == 0 &&
          s->preload == OS64_DOM_MEDIA_PRELOAD_AUTO && s->cors == OS64_DOM_MEDIA_NO_CORS && s->gain == 1,
          "media: the session is for the resolved address, preload auto, no-cors, full volume");
    media_script(&f, "const h=heard();assert(h==='loadstart:0','loadstart only: '+h);"
                     "assert(a.networkState===2&&a.currentSrc==='https://example.test/dir/song.mp3');",
                 "media: loadstart");
    media_ready(s, 3);
    media_turn(&f, "media: the probe");
    media_script(&f, "const h=heard();assert(h==='durationchange:4 loadedmetadata:4 progress:4 loadeddata:4 canplay:4 "
                     "canplaythrough:4','the ladder: '+h);"
                     "assert(a.duration===3&&a.readyState===4&&a.seekable.length===1&&a.seekable.end(0)===3);",
                 "media: metadata to enough");

    /* play() with sound needs the person; muted does not. */
    media_script(&f, "globalThis.R=[];a.play().then(()=>R.push('ok'),e=>R.push(e.name));",
                 "media: play without activation");
    media_turn(&f, "media: nothing queued");
    media_script(&f, "assert(R.join()==='NotAllowedError'&&a.paused&&heard()==='','refused: '+R.join());",
                 "media: play is refused NotAllowedError");
    check(!s->playing, "media: the host was not asked to play");
    media_activated = true;
    media_script(&f, "R.length=0;a.play().then(()=>R.push('ok:'+L.join()),e=>R.push(e.name));"
                     "assert(!a.paused,'play() unpauses at once');",
                 "media: play");
    check(s->playing, "media: the host plays");
    media_turn(&f, "media: play's tasks");
    media_script(&f, "assert(R.join()==='ok:play:4,playing:4','the promise after playing: '+R.join());"
                     "heard();",
                 "media: play, playing, then the promise");

    /* Time moves while it sounds: a timeupdate each 250 ms, not oftener. */
    s->facts.sounding = true;
    s->facts.position = 0.5;
    media_turn(&f, "media: sounding");
    p2b_advance(100);
    s->facts.position = 0.6;
    media_turn(&f, "media: 100 ms later");
    p2b_advance(200);
    s->facts.position = 0.8;
    media_turn(&f, "media: 300 ms later");
    media_script(&f, "const h=heard();assert(h==='timeupdate:4 timeupdate:4','two timeupdates: '+h);"
                     "assert(Math.abs(a.currentTime-0.8)<1e-9,'the host\\'s position: '+a.currentTime);",
                 "media: timeupdate's pace");

    /* A seek: seeking now, timeupdate and seeked when the host lands. */
    media_script(&f, "a.currentTime=2;assert(a.seeking&&a.currentTime===2);", "media: seek");
    check(s->seek_to == 2 && s->seek_generation == 1, "media: the host seeks");
    media_turn(&f, "media: seeking fires");
    s->facts.seeked = 1;
    s->facts.position = 2;
    media_turn(&f, "media: the host landed");
    media_script(&f, "const h=heard();assert(h==='seeking:4 timeupdate:4 seeked:4','the seek: '+h);"
                     "assert(!a.seeking);",
                 "media: seeking, then seeked");

    /* The device another program's: waiting, readyState down to 2. */
    s->facts.busy = true;
    s->facts.sounding = false;
    media_turn(&f, "media: busy");
    media_script(&f, "const h=heard();assert(h==='timeupdate:2 waiting:2','busy waits: '+h);", "media: busy");
    s->facts.busy = false;
    media_turn(&f, "media: the device is free");
    media_script(&f, "const h=heard();assert(h.startsWith('canplay:4 playing:4'),'free plays: '+h);heard()",
                 "media: free again");

    /* Volume and muted reach the host. */
    media_script(&f, "a.volume=0.25;a.volume=0.25;a.muted=true;", "media: volume");
    check(s->gain == 0, "media: muted is a gain of 0");
    media_script(&f, "a.muted=false;", "media: unmuted");
    check(s->gain == 0.25, "media: the volume is the gain");
    media_turn(&f, "media: volumechange");
    media_script(&f, "const h=heard();assert(h==='volumechange:4 volumechange:4 volumechange:4','three changes: '+h);",
                 "media: volumechange once per change");

    /* The end: timeupdate, pause, ended. In no document, the element then
     * gives its session back, and play() wakes it with a new one at the
     * start, its volume carried. */
    s->facts.ended = true;
    s->facts.position = 3;
    media_turn(&f, "media: the end");
    media_script(&f, "const h=heard();assert(h==='timeupdate:4 pause:4 ended:4','the end: '+h);"
                     "assert(a.ended&&a.paused&&a.currentTime===3);",
                 "media: ended");
    check(!s->playing, "media: the end pauses the host too, so a seek back stays silent");
    check(!s->open && media_closed == 1, "media: an ended element in no document gives its session back");
    media_script(&f, "a.play();assert(!a.ended&&a.seeking,'play() after the end seeks to the start');",
                 "media: play() wakes it");
    MediaSession *again = &media_sessions[1];
    check(media_opened == 2 && again->open && again->playing && again->gain == 0.25 &&
          again->seek_generation == 2 && again->seek_to == 0 &&
          again->preload == OS64_DOM_MEDIA_PRELOAD_AUTO,
          "media: a new session, playing, the seek to 0 and the volume carried");
    fixture_free(&f);
    check(media_closed == 2, "media: the teardown closed the second session");
}

static void dom_media_abort_cases(void)
{
    DomFixture f;
    media_fixture(&f);
    if (!fixture_ready(&f)) return;
    media_activated = true;
    /* A pause before playing rejects play()'s promise with AbortError. */
    media_script(&f, "globalThis.a=new Audio('a.mp3');hear(a);globalThis.R=[];"
                     "a.play().then(()=>R.push('ok'),e=>R.push(e.name));",
                 "media: play before metadata");
    media_turn(&f, "media: open");
    media_script(&f, "a.pause();", "media: pause");
    media_turn(&f, "media: the pause's tasks");
    media_script(&f, "assert(R.join()==='AbortError','pause rejects: '+R.join());"
                     "const h=heard();assert(h==='play:0 waiting:0 loadstart:0 timeupdate:0 pause:0','events: '+h);",
                 "media: pause rejects AbortError");

    /* A new src while loaded: abort, emptied, the old session closed. */
    media_ready(&media_sessions[0], 5);
    media_turn(&f, "media: loaded");
    media_script(&f, "heard();a.src='b.mp3';assert(a.readyState===0&&a.networkState===3&&isNaN(a.duration));",
                 "media: a new src");
    check(!media_sessions[0].open && media_closed == 1, "media: the old session closed");
    media_turn(&f, "media: the new one");
    check(media_opened == 2 && strcmp(media_sessions[1].url, "https://example.test/dir/b.mp3") == 0,
          "media: and the new one opened");
    media_script(&f, "const h=heard();assert(h==='abort:0 emptied:0 loadstart:0','the reload: '+h);",
                 "media: abort, emptied, loadstart");

    /* A src the host cannot open: error, code 4, and play() refused. */
    media_refuse = true;
    media_script(&f, "a.src='c.mp3';R.length=0;", "media: a src that will not open");
    media_turn(&f, "media: it fails");
    media_script(&f, "const h=heard();assert(h.endsWith('loadstart:0 error:0'),'error: '+h);"
                     "assert(a.error instanceof MediaError&&a.error.code===4&&a.error===a.error&&a.networkState===3);"
                     "a.play().then(()=>R.push('ok'),e=>R.push(e.name));",
                 "media: MEDIA_ERR_SRC_NOT_SUPPORTED");
    media_turn(&f, "media: settle");
    media_script(&f, "assert(R.join()==='NotSupportedError','play refused: '+R.join());", "media: NotSupportedError");
    fixture_free(&f);
}

static void dom_media_document_cases(void)
{
    DomFixture f;
    media_fixture(&f);
    if (!fixture_ready(&f)) return;
    /* An <audio> in the document: the walk finds it and loads it, preload
     * metadata by default; removed while playing, it pauses. Muted, it may
     * play without the person. */
    media_script(&f, "globalThis.a=document.createElement('audio');a.setAttribute('src','x.mp3');"
                     "a.setAttribute('muted','');document.body.appendChild(a);hear(a);",
                 "media: one in the document");
    media_turn(&f, "media: the walk finds it");
    check(media_opened == 1 && media_sessions[0].preload == OS64_DOM_MEDIA_PRELOAD_METADATA,
          "media: a parsed element loads, preload metadata");
    media_ready(&media_sessions[0], 2);
    media_turn(&f, "media: ready");
    media_script(&f, "heard();globalThis.R=[];a.play().then(()=>R.push('ok'),e=>R.push(e.name));", "media: muted plays");
    media_turn(&f, "media: playing");
    media_script(&f, "assert(R.join()==='ok','muted may play: '+R.join());heard();a.remove();", "media: removed");
    media_turn(&f, "media: the walk sees it go");
    media_script(&f, "const h=heard();assert(h==='timeupdate:4 pause:4'&&a.paused,'removal pauses: '+h);",
                 "media: removal pauses");

    /* Autoplay: muted autoplay starts once it can play through. */
    media_script(&f, "globalThis.b=document.createElement('audio');b.autoplay=true;b.muted=true;b.src='y.mp3';"
                     "document.body.appendChild(b);globalThis.B=[];b.addEventListener('play',()=>B.push('play'));",
                 "media: autoplay");
    media_turn(&f, "media: open the autoplay one");
    media_ready(&media_sessions[1], 2);
    media_turn(&f, "media: it can play through");
    media_script(&f, "assert(B.join()==='play'&&!b.paused,'muted autoplay plays');", "media: autoplay plays");
    check(media_sessions[1].playing, "media: the host was told to play");
    check(media_sessions[1].preload == OS64_DOM_MEDIA_PRELOAD_AUTO,
          "media: autoplay asks for the whole of it, whatever preload says");
    /* With sound and no activation it does not. */
    media_script(&f, "globalThis.c=document.createElement('audio');c.autoplay=true;c.src='z.mp3';"
                     "document.body.appendChild(c);",
                 "media: autoplay with sound");
    media_turn(&f, "media: open it");
    media_ready(&media_sessions[2], 2);
    media_turn(&f, "media: ready");
    media_script(&f, "assert(c.paused,'autoplay with sound waits for the person');", "media: autoplay refused");

    /* <source> children: the first whose type plays. */
    media_script(&f, "globalThis.d=document.createElement('audio');"
                     "d.innerHTML='<source src=\"v.webm\" type=\"video/webm\"><source src=\"w.mp3\" type=\"audio/mpeg\">';"
                     "document.body.appendChild(d);",
                 "media: sources");
    media_turn(&f, "media: the walk loads the second");
    check(media_opened == 4 && strcmp(media_sessions[3].url, "https://example.test/dir/w.mp3") == 0,
          "media: the source whose type plays");
    media_script(&f, "assert(d.currentSrc==='https://example.test/dir/w.mp3');", "media: currentSrc is the source's");

    /* Two playable candidates that both fail while loading: each failure
     * moves on, and after the last the element has no source. */
    media_script(&f, "globalThis.e=document.createElement('audio');"
                     "e.innerHTML='<source src=\"p.mp3\" type=\"audio/mpeg\"><source src=\"q.mp3\" type=\"audio/mpeg\">';"
                     "document.body.appendChild(e);",
                 "media: two failing sources");
    media_turn(&f, "media: the first opens");
    check(media_opened == 5, "media: the first candidate opened");
    media_sessions[4].facts.error = OS64_DOM_MEDIA_ERR_SRC_NOT_SUPPORTED;
    media_turn(&f, "media: the first fails");
    check(media_opened == 6 && strcmp(media_sessions[5].url, "https://example.test/dir/q.mp3") == 0,
          "media: on to the second");
    media_sessions[5].facts.error = OS64_DOM_MEDIA_ERR_SRC_NOT_SUPPORTED;
    media_turn(&f, "media: the second fails");
    media_script(&f, "assert(e.networkState===3,'no source left: '+e.networkState);",
                 "media: the second's failure is heard too");

    /* A seek a script asks of a parsed element before its first turn is
     * kept through the load the turn starts. */
    media_script(&f, "globalThis.g=document.createElement('audio');g.setAttribute('src','r.mp3');"
                     "document.body.appendChild(g);g.currentTime=1.5;",
                 "media: a seek before the first turn");
    media_turn(&f, "media: the first turn");
    media_ready(&media_sessions[6], 3);
    media_turn(&f, "media: metadata");
    check(media_sessions[6].seek_generation == 1 && media_sessions[6].seek_to == 1.5,
          "media: the early seek is made once the length is known");
    fixture_free(&f);
    check(media_closed == 7, "media: every session closed at teardown");
}

static void dom_media_loop_cases(void)
{
    DomFixture f;
    media_fixture(&f);
    if (!fixture_ready(&f)) return;
    media_activated = true;
    media_script(&f, "globalThis.a=new Audio('l.mp3');hear(a);", "media: a clip");
    media_turn(&f, "media: open it");
    MediaSession *s = &media_sessions[0];
    media_ready(s, 1);
    media_turn(&f, "media: ready");
    media_script(&f, "a.play();", "media: play it");
    media_turn(&f, "media: playing");
    /* Loop turned on after the host decoded to the end: the host reports
     * the end, and the element seeks to the start (HTML's loop step), so
     * it neither stops nor sticks. */
    media_script(&f, "heard();a.loop=true;", "media: loop on late");
    s->facts.ended = true;
    media_turn(&f, "media: the end with loop on");
    media_script(&f, "const h=heard();assert(h.startsWith('seeking:4'),'loop seeks: '+h);"
                     "assert(!a.paused&&!a.ended&&a.seeking);",
                 "media: a late loop seeks to the start");
    check(s->seek_to == 0 && s->seek_generation == 1 && s->loop, "media: the host seeks to 0 and loops from now on");
    s->facts.ended = false;
    s->facts.seeked = 1;
    s->facts.position = 0;
    media_turn(&f, "media: landed");
    /* A resource the host cannot seek is not sought. */
    s->facts.seekable = false;
    media_script(&f, "heard();a.currentTime=0.5;assert(!a.seeking,'no seek on what cannot seek');",
                 "media: unseekable");
    check(s->seek_generation == 1, "media: the host was not asked to seek");
    fixture_free(&f);
}

static void dom_media_cases(void)
{
    dom_media_interface_cases();
    dom_media_play_cases();
    dom_media_abort_cases();
    dom_media_document_cases();
    dom_media_loop_cases();
}
