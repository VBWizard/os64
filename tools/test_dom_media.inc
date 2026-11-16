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
    bool loop, all;
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
static void media_settings(void *opaque, void *session, double gain, bool loop, bool all)
{
    (void)opaque;
    ((MediaSession *)session)->gain = gain;
    ((MediaSession *)session)->loop = loop;
    ((MediaSession *)session)->all = all;
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
                     "a.muted=true;document.body.appendChild(a);hear(a);",
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
    media_script(&f, "const z=document.createElement('audio');z.src='z2.mp3';z.removeAttribute('src');z.load();"
                     "assert(z.currentSrc===''&&z.networkState===3,'no resource selected: '+z.currentSrc);",
                 "media: load() with no source clears currentSrc");

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
    /* A <source> added once every candidate has failed resumes the
     * selection there. */
    media_script(&f, "const s=document.createElement('source');s.src='t.mp3';s.type='audio/mpeg';e.appendChild(s);"
                     "assert(s.src==='https://example.test/dir/t.mp3','source.src reflects, resolved');",
                 "media: a new source after the failures");
    media_turn(&f, "media: the new candidate");
    check(media_opened == 7 && strcmp(media_sessions[6].url, "https://example.test/dir/t.mp3") == 0,
          "media: the appended source is tried");

    /* A seek a script asks of a parsed element before its first turn is
     * kept through the load the turn starts. */
    media_script(&f, "globalThis.g=document.createElement('audio');g.setAttribute('src','r.mp3');"
                     "document.body.appendChild(g);g.currentTime=1.5;",
                 "media: a seek before the first turn");
    media_turn(&f, "media: the first turn");
    media_ready(&media_sessions[7], 3);
    media_turn(&f, "media: metadata");
    check(media_sessions[7].seek_generation == 1 && media_sessions[7].seek_to == 1.5,
          "media: the early seek is made once the length is known");
    fixture_free(&f);
    check(media_closed == 8, "media: every session closed at teardown");
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

static void dom_media_rule_cases(void)
{
    DomFixture f;
    media_fixture(&f);
    if (!fixture_ready(&f)) return;
    /* load() and play() on a parsed element before its first turn: one
     * load, and play()'s promise is not rejected by a second. */
    /* Before metadata, currentTime is the seek asked for. */
    media_script(&f, "const c=document.createElement('audio');c.currentTime=1.5;assert(c.currentTime===1.5,'pending: '+c.currentTime);"
                     "c.currentTime+=1;assert(c.currentTime===2.5,'added to: '+c.currentTime);",
                 "media: currentTime before metadata");
    media_script(&f, "globalThis.a=document.createElement('audio');a.setAttribute('src','s.mp3');a.muted=true;"
                     "document.body.appendChild(a);hear(a);globalThis.R=[];a.load();"
                     "a.play().then(()=>R.push('ok'),e=>R.push(e.name));",
                 "media: load and play before the first turn");
    media_turn(&f, "media: the first turn");
    media_ready(&media_sessions[0], 2);
    media_turn(&f, "media: ready");
    check(media_opened == 1, "media: one session, not two");
    media_script(&f, "const h=heard();assert(R.join()==='ok','not rejected: '+R.join());"
                     "assert(h.indexOf('abort')<0&&h.split('loadstart').length===2,'one selection: '+h);",
                 "media: one load");
    /* setAttribute('src') loads at once, as the property does: a play()
     * right after it plays the new resource, and a detached element given
     * a src that way loads with no media member touched. */
    media_script(&f, "heard();a.setAttribute('src','s2.mp3');assert(a.readyState===0&&a.networkState===3,'loaded at once');"
                     "R.length=0;a.play().then(()=>R.push('ok'),e=>R.push(e.name));",
                 "media: setAttribute src then play");
    media_turn(&f, "media: the new resource opens");
    MediaSession *s2 = &media_sessions[media_opened - 1];
    check(strcmp(s2->url, "https://example.test/dir/s2.mp3") == 0 && s2->playing,
          "media: the new resource opened, playing");
    media_ready(s2, 2);
    media_turn(&f, "media: ready");
    media_script(&f, "assert(R.join()==='ok'&&!a.paused,'play() is for the new resource: '+R.join());heard();",
                 "media: play resolves for it");
    media_script(&f, "globalThis.d=document.createElement('audio');globalThis.D=[];"
                     "d.addEventListener('loadstart',()=>D.push('loadstart'));d.setAttribute('src','d.mp3');",
                 "media: a detached element by attribute");
    media_turn(&f, "media: it loads");
    media_script(&f, "assert(D.join()==='loadstart','loads: '+D.join());", "media: the detached element loads");
    /* Muted let it play without the person; unmuted, still without them,
     * it pauses rather than sound. */
    media_script(&f, "heard();a.muted=false;assert(a.paused&&!a.muted,'unmuting without the person pauses');",
                 "media: unmute without activation");
    check(!s2->playing, "media: the host was told to pause");
    media_activated = true;
    media_script(&f, "a.play();a.muted=true;a.muted=false;assert(!a.paused,'with the person it plays on');",
                 "media: unmute with activation");

    /* preload=none defers the load; preload set to auto later resumes it,
     * once, with no second loadstart, and a metadata session told so. */
    media_script(&f, "globalThis.n=document.createElement('audio');n.preload='none';n.src='n.mp3';"
                     "document.body.appendChild(n);globalThis.N=[];"
                     "n.addEventListener('loadstart',()=>N.push('loadstart'));"
                     "n.addEventListener('suspend',()=>N.push('suspend'));",
                 "media: preload none");
    size_t before = media_opened;
    media_turn(&f, "media: deferred");
    check(media_opened == before, "media: preload none opens nothing");
    media_script(&f, "n.preload='auto';", "media: preload auto");
    media_turn(&f, "media: resumed");
    check(media_opened == before + 1 && media_sessions[before].preload == OS64_DOM_MEDIA_PRELOAD_AUTO,
          "media: the deferred load goes on, the whole wanted");
    media_script(&f, "assert(N.join()==='loadstart,suspend','one loadstart: '+N.join());", "media: one loadstart");
    media_script(&f, "globalThis.k=document.createElement('audio');k.src='k.mp3';document.body.appendChild(k);",
                 "media: a metadata element");
    media_turn(&f, "media: opened for metadata");
    MediaSession *k = &media_sessions[media_opened - 1];
    check(k->preload == OS64_DOM_MEDIA_PRELOAD_METADATA && !k->all, "media: for its metadata");
    media_script(&f, "k.preload='auto';", "media: and then auto");
    media_turn(&f, "media: told");
    check(k->all, "media: the host is told the whole is wanted");

    /* A looping element whose sound ended at a failure is no loop point:
     * it ends instead of seeking back to the same failure. */
    media_script(&f, "globalThis.l=new Audio('lf.mp3');l.loop=true;l.play();", "media: a looping element");
    media_turn(&f, "media: open it");
    MediaSession *ls = &media_sessions[media_opened - 1];
    media_ready(ls, 2);
    media_turn(&f, "media: playing");
    ls->facts.error = OS64_DOM_MEDIA_ERR_NETWORK;
    media_turn(&f, "media: it fails part way");
    ls->facts.ended = true;
    media_turn(&f, "media: its sound runs out");
    media_script(&f, "assert(l.paused&&!l.seeking&&l.error.code===2,'it stops, no loop');", "media: no loop after failure");
    check(ls->seek_generation == 0, "media: no seek back");
    fixture_free(&f);
}

static const os64_html_node_t *media_by_id(const os64_html_node_t *node, const char *id)
{
    for (; node != NULL; node = node->next) {
        const os64_html_attr_t *attr = node->kind == OS64_HTML_ELEMENT ? os64_html_attr(node, "id") : NULL;
        if (attr != NULL && strcmp(attr->value, id) == 0) return node;
        const os64_html_node_t *inside = media_by_id(node->first_child, id);
        if (inside != NULL) return inside;
    }
    return NULL;
}

/* The controls (§ The controls): what they show is the element's members,
 * and what they do is what a script's call would do, events and all. A
 * refused play() is the element's state, not the page's rejection. */
static void dom_media_controls_cases(void)
{
    DomFixture f;
    media_fixture(&f);
    if (!fixture_ready(&f)) return;
    media_script(&f, "globalThis.a=document.createElement('audio');a.id='player';a.controls=true;"
                     "a.src='song.mp3';document.body.appendChild(a);hear(a);"
                     "globalThis.U=[];addEventListener('unhandledrejection',e=>U.push(e.reason&&e.reason.name));",
                 "controls: an element");
    const os64_html_node_t *node = media_by_id(f.document->document, "player");
    os64_dom_media_view_t view;
    check(node != NULL && os64_dom_media_view(f.binding, node, &view) && view.paused && !view.ended &&
          isnan(view.duration) && view.position == 0 && view.volume == 1 && !view.muted,
          "controls: the view of an element not yet loaded");
    media_turn(&f, "controls: the turn opens it");
    MediaSession *s = &media_sessions[0];
    media_ready(s, 4);
    media_turn(&f, "controls: the probe");
    media_script(&f, "heard();", "controls: the ladder heard");
    check(os64_dom_media_view(f.binding, node, &view) && view.duration == 4 && view.seekable && view.arrived == 4,
          "controls: the view has the length, what arrived and that it seeks");
    // After a seek far ahead, what has arrived begins where the new fetch did.
    s->facts.arrived_from = 1.5;
    media_script(&f, "assert(a.buffered.length===1&&a.buffered.start(0)===1.5&&a.buffered.end(0)===4,"
                     "'buffered starts where the bytes do');",
                 "controls: buffered after a seek far ahead");
    check(os64_dom_media_view(f.binding, node, &view) && view.arrived_from == 1.5 && view.arrived == 4,
          "controls: the view has where the arrived run begins");
    s->facts.arrived_from = 0;

    os64_js_outcome_t out;
    check(os64_dom_media_control(f.binding, node, OS64_DOM_MEDIA_PLAY, 0, &out) && out.status == OS64_JS_OK,
          "controls: a play without the person runs");
    media_turn(&f, "controls: nothing follows a refused play");
    check(!s->playing && os64_dom_media_view(f.binding, node, &view) && view.paused,
          "controls: a play without the person is refused, as script's is");
    media_activated = true;
    check(os64_dom_media_control(f.binding, node, OS64_DOM_MEDIA_PLAY, 0, &out) && out.status == OS64_JS_OK,
          "controls: a play with the person runs");
    media_turn(&f, "controls: play's events");
    check(s->playing && os64_dom_media_view(f.binding, node, &view) && !view.paused,
          "controls: the host plays and the view says so");
    check(os64_dom_media_control(f.binding, node, OS64_DOM_MEDIA_SEEK, 2.5, &out) &&
          os64_dom_media_control(f.binding, node, OS64_DOM_MEDIA_VOLUME, 0.25, &out) &&
          os64_dom_media_control(f.binding, node, OS64_DOM_MEDIA_MUTE, 1, &out) &&
          os64_dom_media_control(f.binding, node, OS64_DOM_MEDIA_PAUSE, 0, &out),
          "controls: a seek, the volume, mute and a pause");
    check(s->seek_to == 2.5 && !s->playing && s->gain == 0, "controls: the host was told each");
    media_turn(&f, "controls: their events");
    media_script(&f, "const h=heard();"
                     "assert(h==='play:4 playing:4 seeking:4 volumechange:4 volumechange:4 timeupdate:4 pause:4',"
                     "'the events: '+h);"
                     "assert(a.volume===0.25&&a.muted&&a.paused&&a.currentTime===2.5,'the members agree');",
                 "controls: the page hears what script's calls would fire");
    media_script(&f, "assert(U.length===0,'no unhandled rejection: '+U.join());", "controls: the refusal was not the page's");
    check(os64_dom_media_control(f.binding, node, OS64_DOM_MEDIA_VOLUME, 7, &out) &&
          os64_dom_media_view(f.binding, node, &view) && view.volume == 1,
          "controls: a volume past the end is held to it");
    fixture_free(&f);
}

/* A <video> (libdom/video.c; AUDIO_YONDER.md § Y.4): a media element in
 * full, playing through the same sessions, with its own members and no
 * picture. */
static void dom_media_video_cases(void)
{
    DomFixture f;
    media_fixture(&f);
    if (!fixture_ready(&f)) return;
    media_script(&f,
        "assert(Object.getPrototypeOf(HTMLVideoElement)===HTMLMediaElement,'HTMLVideoElement is a media element');"
        "const v=document.createElement('video');"
        "assert(v instanceof HTMLVideoElement&&v instanceof HTMLMediaElement&&!(v instanceof HTMLAudioElement));"
        "assert(String(v)==='[object HTMLVideoElement]');throws('TypeError',()=>new HTMLVideoElement());"
        "assert(v.width===0&&v.height===0&&v.poster===''&&!v.playsInline,'the members start empty');"
        "v.width=640;v.height=360;v.playsInline=true;v.poster='still.png';"
        "assert(v.getAttribute('width')==='640'&&v.getAttribute('height')==='360'&&v.hasAttribute('playsinline'),'reflected');"
        "assert(v.poster==='https://example.test/dir/still.png','a poster resolves: '+v.poster);"
        "v.setAttribute('width','-3');assert(v.width===0,'an unsigned reflection');"
        "assert(v.videoWidth===0&&v.videoHeight===0,'no picture');"
        "assert(!Object.getOwnPropertyDescriptor(HTMLVideoElement.prototype,'videoWidth').set,'videoWidth is read-only');"
        "throws('TypeError',()=>Object.getOwnPropertyDescriptor(HTMLVideoElement.prototype,'width').get.call(new Audio()));"
        "assert(v.canPlayType('audio/mpeg')==='maybe'&&v.canPlayType('video/mp4')==='','canPlayType is the host\\'s (this fake host plays only MP3)');"
        "assert(v.paused&&v.networkState===0&&v.readyState===0&&isNaN(v.duration),'a media element\\'s start');",
        "media: video: the interface and its members");
    /* JW Player's way: a video element given an MP3, in the document. */
    media_script(&f, "globalThis.a=document.createElement('video');hear(a);a.src='episode.mp3';document.body.append(a);",
                 "media: video: given a sound");
    media_turn(&f, "media: video: the turn opens it");
    MediaSession *s = &media_sessions[0];
    check(media_opened == 1 && s->open && strcmp(s->url, "https://example.test/dir/episode.mp3") == 0,
          "media: video: a session for the sound, as an audio's");
    media_ready(s, 5);
    media_turn(&f, "media: video: the probe");
    media_script(&f, "const h=heard();assert(h.endsWith('canplay:4 canplaythrough:4'),'the ladder: '+h);"
                     "assert(a.duration===5&&a.videoWidth===0,'its length, and still no picture');",
                 "media: video: metadata to enough");
    media_activated = true;
    media_script(&f, "globalThis.R=[];a.play().then(()=>R.push('ok'),e=>R.push(e.name));",
                 "media: video: play");
    check(s->playing, "media: video: the host plays it");
    media_turn(&f, "media: video: play's tasks");
    media_script(&f, "assert(R.join()==='ok'&&!a.paused,'the promise: '+R.join());a.pause();assert(a.paused);",
                 "media: video: played and paused");
    check(!s->playing, "media: video: the host pauses it");
    fixture_free(&f);
}

/* THE MEDIA SESSION (libdom/media_session.c; AUDIO_YONDER.md § Y.6): the
 * interface and its TypeErrors, MediaMetadata, the window's state the host
 * reads, a press done (the page's handler, or the browser's own answer),
 * and a handler that replaces or removes itself while it is called. */
static void dom_media_session_cases(void)
{
    DomFixture f;
    media_fixture(&f);
    if (!fixture_ready(&f)) return;
    media_script(&f,
        "const s=navigator.mediaSession;"
        "assert(s instanceof MediaSession&&s===navigator.mediaSession,'one MediaSession, the same each time');"
        "assert(String(s)==='[object MediaSession]');throws('TypeError',()=>new MediaSession());"
        "assert(Object.getOwnPropertyDescriptor(Navigator.prototype,'mediaSession').get,'a member of Navigator.prototype');"
        "assert(s.playbackState==='none'&&s.metadata===null,'the start');"
        "s.playbackState='paused';assert(s.playbackState==='paused');"
        "s.playbackState='sleeping';assert(s.playbackState==='paused','a value outside the enum is ignored');"
        "s.playbackState='none';"
        "throws('TypeError',()=>{s.metadata=5});throws('TypeError',()=>{s.metadata={title:'x'}});"
        "throws('TypeError',()=>s.setActionHandler('dance',()=>{}));"
        "throws('TypeError',()=>s.setActionHandler('play',5));throws('TypeError',()=>s.setActionHandler('play'));"
        "s.setActionHandler('play',null);s.setActionHandler('seekto',undefined);"
        "s.setPositionState();s.setPositionState({});"
        "throws('TypeError',()=>s.setPositionState({position:1}));"
        "throws('TypeError',()=>s.setPositionState({duration:-1}));"
        "throws('TypeError',()=>s.setPositionState({duration:10,position:11}));"
        "throws('TypeError',()=>s.setPositionState({duration:10,playbackRate:0}));"
        "throws('TypeError',()=>s.setPositionState({duration:10,position:NaN}));"
        "s.setPositionState({duration:Infinity,position:5});s.setPositionState({duration:10,position:10,playbackRate:-1});"
        "assert(s.setMicrophoneActive(true)===undefined&&s.setCameraActive(false)===undefined);",
        "session: the interface");
    media_script(&f,
        "const e=new MediaMetadata();"
        "assert(e.title===''&&e.artist===''&&e.album===''&&e.artwork.length===0,'empty by default');"
        "const m=new MediaMetadata({title:'Episode 7',artist:'Two Hosts',album:'The Show',"
        "artwork:[{src:'cover.png',sizes:'96x96',type:'image/png'},{src:'https://cdn.test/big.jpg'}]});"
        "assert(m.title==='Episode 7'&&m.artist==='Two Hosts'&&m.album==='The Show','the strings');"
        "assert(m.artwork.length===2&&m.artwork[0].src==='https://example.test/dir/cover.png'&&"
        "m.artwork[0].sizes==='96x96'&&m.artwork[0].type==='image/png','artwork resolved: '+m.artwork[0].src);"
        "assert(m.artwork[1].sizes===''&&m.artwork[1].type==='','a MediaImage\\'s defaults');"
        "assert(Object.isFrozen(m.artwork)&&Object.isFrozen(m.artwork[0]),'artwork is frozen');"
        "throws('TypeError',()=>new MediaMetadata({artwork:[{sizes:'1x1'}]}));"
        "throws('TypeError',()=>new MediaMetadata({artwork:[{src:'http://[nope'}]}));"
        "throws('TypeError',()=>new MediaMetadata({artwork:5}));"
        "m.title=42;assert(m.title==='42','a DOMString');"
        "m.artwork=new Set([{src:'other.png'}]);assert(m.artwork[0].src==='https://example.test/dir/other.png','any sequence');"
        "throws('TypeError',()=>{m.artwork=[{src:'http://[nope'}]});"
        "assert(m.artwork[0].src==='https://example.test/dir/other.png','a refused assignment changes nothing');"
        "navigator.mediaSession.metadata=m;assert(navigator.mediaSession.metadata===m);"
        "navigator.mediaSession.metadata=null;assert(navigator.mediaSession.metadata===null);"
        "throws('TypeError',()=>MediaMetadata());",
        "session: MediaMetadata");

    /* The window's state, before and after the page says anything. */
    os64_dom_media_session_t st;
    check(os64_dom_media_session_state(f.binding, &st) && st.state == OS64_DOM_MEDIA_SESSION_STOPPED && !st.wanted,
          "session: a page that said nothing is stopped and wants no session");
    media_script(&f, "navigator.mediaSession.playbackState='playing';", "session: declared playing");
    check(os64_dom_media_session_state(f.binding, &st) && st.state == OS64_DOM_MEDIA_SESSION_PLAYING && st.wanted,
          "session: declared playing is playing");
    media_script(&f, "navigator.mediaSession.playbackState='paused';", "session: declared paused");
    check(os64_dom_media_session_state(f.binding, &st) && st.state == OS64_DOM_MEDIA_SESSION_PAUSED,
          "session: declared paused is paused");
    media_script(&f, "navigator.mediaSession.playbackState='none';", "session: declared none");

    /* A press with a handler: the handler, given its action. */
    media_script(&f, "globalThis.R=[];"
                     "navigator.mediaSession.setActionHandler('nexttrack',d=>R.push(d.action));"
                     "navigator.mediaSession.setActionHandler('previoustrack',function(d){R.push('p:'+d.action+':'+Object.keys(d).join());"
                     "navigator.mediaSession.setActionHandler('previoustrack',()=>R.push('second'))});"
                     "navigator.mediaSession.setActionHandler('stop',()=>{R.push('stop');navigator.mediaSession.setActionHandler('stop',null)});",
                 "session: handlers");
    check(os64_dom_media_session_state(f.binding, &st) && st.wanted && st.state == OS64_DOM_MEDIA_SESSION_STOPPED,
          "session: a handler makes a page want a session");
    os64_js_outcome_t out;
    check(os64_dom_media_session_action(f.binding, OS64_DOM_MEDIA_SESSION_NEXT, &out) && out.status == OS64_JS_OK,
          "session: next runs");
    check(os64_dom_media_session_action(f.binding, OS64_DOM_MEDIA_SESSION_NEXT, &out) && out.status == OS64_JS_OK,
          "session: next again");
    check(os64_dom_media_session_action(f.binding, OS64_DOM_MEDIA_SESSION_PREVIOUS, &out) &&
          os64_dom_media_session_action(f.binding, OS64_DOM_MEDIA_SESSION_PREVIOUS, &out) &&
          os64_dom_media_session_action(f.binding, OS64_DOM_MEDIA_SESSION_STOP, &out) &&
          os64_dom_media_session_action(f.binding, OS64_DOM_MEDIA_SESSION_STOP, &out) && out.status == OS64_JS_OK,
          "session: previous twice, stop twice");
    media_script(&f, "assert(R.join()==='nexttrack,nexttrack,p:previoustrack:action,second,stop',"
                     "'each handler, the one set during a call next time, a removed one never: '+R.join());",
                 "session: the handlers ran as set");
    media_script(&f, "globalThis.E=[];addEventListener('error',e=>E.push(e.message));"
                     "navigator.mediaSession.setActionHandler('nexttrack',()=>{throw new Error('broken handler')});",
                 "session: a handler that throws");
    (void)os64_dom_media_session_action(f.binding, OS64_DOM_MEDIA_SESSION_NEXT, &out);
    media_script(&f, "assert(E.length===1&&E[0].indexOf('broken handler')>=0,'the window heard it: '+E.join());",
                 "session: a handler's exception is the window's error event");

    /* With no handler, the browser's answer: two elements, the last played
     * is the one Play plays; Pause and Stop pause every one. */
    media_script(&f, "navigator.mediaSession.setActionHandler('nexttrack',null);"
                     "globalThis.a=new Audio('one.mp3');globalThis.b=new Audio('two.mp3');",
                 "session: two elements");
    media_turn(&f, "session: their sessions");
    /* Each element's session by its address: the turn opens them in its
     * own order. */
    MediaSession *sa = NULL, *sb = NULL;
    for (size_t i = 0; i < media_opened; i++) {
        if (strstr(media_sessions[i].url, "one.mp3") != NULL) sa = &media_sessions[i];
        if (strstr(media_sessions[i].url, "two.mp3") != NULL) sb = &media_sessions[i];
    }
    check(sa != NULL && sb != NULL && sa != sb, "session: a session for each element");
    if (sa == NULL || sb == NULL) {
        fixture_free(&f);
        return;
    }
    media_ready(sa, 30);
    media_ready(sb, 30);
    media_turn(&f, "session: both ready");
    media_activated = true;
    media_script(&f, "a.play();b.play();", "session: both play");
    check(sa->playing && sb->playing && os64_dom_media_session_state(f.binding, &st) &&
          st.state == OS64_DOM_MEDIA_SESSION_PLAYING, "session: an element playing makes the window playing");
    check(os64_dom_media_session_action(f.binding, OS64_DOM_MEDIA_SESSION_PAUSE, &out) && out.status == OS64_JS_OK &&
          !sa->playing && !sb->playing, "session: pause, no handler, pauses every element");
    check(os64_dom_media_session_state(f.binding, &st) && st.state == OS64_DOM_MEDIA_SESSION_PAUSED,
          "session: elements paused after playing make the window paused");
    check(os64_dom_media_session_action(f.binding, OS64_DOM_MEDIA_SESSION_PLAY, &out) && out.status == OS64_JS_OK &&
          sb->playing && !sa->playing, "session: play, no handler, plays the element that played last");
    media_script(&f, "assert(!b.paused&&a.paused,'the page agrees: a '+a.paused+' b '+b.paused);",
                 "session: the elements say so too");
    check(os64_dom_media_session_action(f.binding, OS64_DOM_MEDIA_SESSION_NEXT, &out) && out.status == OS64_JS_OK &&
          sb->playing, "session: next, no handler, does nothing");
    check(os64_dom_media_session_action(f.binding, OS64_DOM_MEDIA_SESSION_STOP, &out) && !sb->playing,
          "session: stop, no handler, pauses");
    media_turn(&f, "session: the events");
    media_script(&f, "R.length=0;navigator.mediaSession.setActionHandler('play',d=>R.push(d.action));",
                 "session: a play handler");
    check(os64_dom_media_session_action(f.binding, OS64_DOM_MEDIA_SESSION_PLAY, &out) && !sa->playing && !sb->playing,
          "session: a page's play handler is asked instead, and plays nothing itself");
    media_script(&f, "assert(R.join()==='play','the page was asked: '+R.join());", "session: the play handler ran");
    fixture_free(&f);
}

static void dom_media_cases(void)
{
    dom_media_session_cases();
    dom_media_video_cases();
    dom_media_controls_cases();
    dom_media_rule_cases();
    dom_media_interface_cases();
    dom_media_play_cases();
    dom_media_abort_cases();
    dom_media_document_cases();
    dom_media_loop_cases();
}
