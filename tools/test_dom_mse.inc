/* MEDIA SOURCE EXTENSIONS (libdom/mse.c; MEDIA_SOURCE.md § 7): the
 * MediaSource, SourceBuffer and SourceBufferList interfaces, their states,
 * the events each step queues and their order, the object URL that
 * attaches a source, and the element's buffered and seekable over it,
 * against a fake fed host (dom/media.h § THE FED SOURCE).
 *
 * The fake's "bytes" are words, so a case says what a segment holds:
 *   init:<a|v|av>:<duration|nan>  an initialization segment
 *   frames:<start>:<end>          coded frames over [start, end) (in
 *                                 sequence mode, placed at the group's end)
 *   media / done                  the parser stops inside a media segment,
 *                                 or leaves it
 *   error                         the byte stream breaks a rule
 *   pending:<n>                   the parse runs elsewhere for n polls
 * The page records what it hears in L as "name:type". */

#define MSE_ASSERT MEDIA_ASSERT \
    "function watch(o,n,ts){for(const t of ts)o.addEventListener(t,()=>L.push(n+':'+t));}" \
    "var SB_EVENTS=['updatestart','update','updateend','error','abort'];" \
    "var MS_EVENTS=['sourceopen','sourceended','sourceclose'];" \
    "var LIST_EVENTS=['addsourcebuffer','removesourcebuffer'];" \
    "var V_EVENTS=['loadstart','durationchange','loadedmetadata','error','emptied','abort'];" \
    "function B(s){return new TextEncoder().encode(s)}" \
    "function R(r){let s=[];for(let i=0;i<r.length;i++)s.push(r.start(i)+'-'+r.end(i));return s.join(',')}"

#define FED_QUOTA (1u << 20)

typedef struct {
    bool live;
    char type[96];
    char input[1024];
    size_t input_len;
    os64_dom_fed_mode_t mode;
    double offset, window_start, window_end, group_end;
    os64_dom_fed_range_t ranges[16];
    uint32_t count;
    double highest;
    bool parsing_media;
    int pending_polls;
    os64_dom_fed_result_t held;     /* what the poll answers once the parse is done */
    int parses, polls, resets, removes, takes, evicts;
} FedBuffer;

typedef struct {
    MediaSession media;             /* first: the media provider's calls take the fed session */
    bool open;
    const os64_html_node_t *node;
    FedBuffer buffers[4];
    uint32_t count;
    double duration;
    int ends;
} FedSession;

static FedSession fed_sessions[4];
static size_t fed_opened, fed_closed;
static bool fed_refuse_add;

static void *fed_open(void *opaque, const os64_html_node_t *node)
{
    (void)opaque;
    if (fed_opened == sizeof(fed_sessions) / sizeof(fed_sessions[0])) return NULL;
    FedSession *s = &fed_sessions[fed_opened++];
    memset(s, 0, sizeof(*s));
    s->open = true;
    s->node = node;
    s->media.open = true;
    s->media.gain = -1;
    s->duration = NAN;
    return s;
}
static void fed_close(void *opaque, void *session)
{
    (void)opaque;
    ((FedSession *)session)->open = false;
    fed_closed++;
}
static void *fed_add(void *opaque, void *session, const char *type)
{
    (void)opaque;
    FedSession *s = session;
    if (fed_refuse_add || s->count == sizeof(s->buffers) / sizeof(s->buffers[0])) return NULL;
    FedBuffer *b = &s->buffers[s->count++];
    memset(b, 0, sizeof(*b));
    b->live = true;
    snprintf(b->type, sizeof(b->type), "%s", type);
    b->window_end = INFINITY;
    b->highest = NAN;
    return b;
}
static void fed_remove_buffer(void *opaque, void *session, void *buffer)
{
    (void)opaque; (void)session;
    ((FedBuffer *)buffer)->live = false;
}
static bool fed_evict(void *opaque, void *session, void *buffer, size_t incoming)
{
    (void)opaque; (void)session;
    ((FedBuffer *)buffer)->evicts++;
    return incoming <= FED_QUOTA;
}
static bool fed_take(void *opaque, void *session, void *buffer, const uint8_t *bytes, size_t length)
{
    (void)opaque; (void)session;
    FedBuffer *b = buffer;
    b->takes++;
    if (b->input_len + length >= sizeof(b->input)) return false;
    memcpy(b->input + b->input_len, bytes, length);
    b->input_len += length;
    b->input[b->input_len] = '\0';
    return true;
}

/* [start, end) into the buffer's ranges, kept sorted and merged. */
static void fed_add_range(FedBuffer *b, double start, double end)
{
    os64_dom_fed_range_t out[17];
    uint32_t n = 0;
    bool placed = false;
    for (uint32_t i = 0; i <= b->count; i++) {
        os64_dom_fed_range_t r;
        if (i < b->count && (placed || b->ranges[i].start <= start)) r = b->ranges[i];
        else if (!placed) { r = (os64_dom_fed_range_t){start, end}; placed = true; i--; }
        else break;
        if (n > 0 && r.start <= out[n - 1].end) {
            if (r.end > out[n - 1].end) out[n - 1].end = r.end;
        } else out[n++] = r;
    }
    if (n > 16) n = 16;
    memcpy(b->ranges, out, sizeof(out[0]) * n);
    b->count = n;
}

static void fed_result_of(FedBuffer *b, os64_dom_fed_result_t *out)
{
    out->timestamp_offset = b->offset;
    out->highest_pts = b->highest;
    out->parsing_media = b->parsing_media;
}

/* The words taken so far, parsed; with `pending:<n>` the rest is parsed at
 * once and held for the poll that ends the wait. */
static void fed_parse(void *opaque, void *session, void *buffer, os64_dom_fed_result_t *out)
{
    (void)opaque; (void)session;
    FedBuffer *b = buffer;
    b->parses++;
    *out = (os64_dom_fed_result_t){.status = OS64_DOM_FED_DONE, .init_duration = NAN, .group_end = NAN};
    int pending = 0;
    char *save = NULL;
    for (char *word = strtok_r(b->input, " ", &save); word != NULL; word = strtok_r(NULL, " ", &save)) {
        char kinds[8];
        double x, y;
        if (sscanf(word, "init:%7[av]:%lf", kinds, &x) == 2 || sscanf(word, "init:%7[av]:nan", kinds) == 1) {
            out->inits++;
            out->audio = strchr(kinds, 'a') != NULL;
            out->video = strchr(kinds, 'v') != NULL;
            out->init_duration = strstr(word, ":nan") != NULL ? NAN : x;
        } else if (sscanf(word, "frames:%lf:%lf", &x, &y) == 2) {
            double start = x + b->offset, end = y + b->offset;
            if (b->mode == OS64_DOM_FED_SEQUENCE) {
                b->offset = b->group_end - x;
                start = b->group_end;
                end = b->group_end + (y - x);
            }
            fed_add_range(b, start, end);
            b->group_end = end;
            if (isnan(b->highest) || end - 0.5 > b->highest) b->highest = end - 0.5;
            out->frames = true;
            out->group_end = end;
        } else if (strcmp(word, "media") == 0) {
            b->parsing_media = true;
        } else if (strcmp(word, "done") == 0) {
            b->parsing_media = false;
        } else if (strcmp(word, "error") == 0) {
            out->status = OS64_DOM_FED_ERROR;
            snprintf(out->message, sizeof(out->message), "the fake's error");
        } else if (sscanf(word, "pending:%d", &pending) == 1) {
        }
    }
    b->input_len = 0;
    b->input[0] = '\0';
    fed_result_of(b, out);
    if (pending > 0) {
        b->held = *out;
        b->pending_polls = pending;
        out->status = OS64_DOM_FED_PENDING;
    }
}
static void fed_poll(void *opaque, void *session, void *buffer, os64_dom_fed_result_t *out)
{
    (void)opaque; (void)session;
    FedBuffer *b = buffer;
    b->polls++;
    if (--b->pending_polls > 0) {
        out->status = OS64_DOM_FED_PENDING;
        return;
    }
    *out = b->held;
}
static void fed_reset(void *opaque, void *session, void *buffer, os64_dom_fed_result_t *out)
{
    (void)opaque; (void)session;
    FedBuffer *b = buffer;
    b->resets++;
    b->input_len = 0;
    b->input[0] = '\0';
    b->parsing_media = false;
    fed_result_of(b, out);
}
static void fed_remove(void *opaque, void *session, void *buffer, double start, double end, os64_dom_fed_result_t *out)
{
    (void)opaque; (void)session;
    FedBuffer *b = buffer;
    b->removes++;
    os64_dom_fed_range_t kept[32];
    uint32_t n = 0;
    for (uint32_t i = 0; i < b->count; i++) {
        os64_dom_fed_range_t r = b->ranges[i];
        if (r.start < start) kept[n++] = (os64_dom_fed_range_t){r.start, r.end < start ? r.end : start};
        if (r.end > end) kept[n++] = (os64_dom_fed_range_t){r.start > end ? r.start : end, r.end};
    }
    if (n > 16) n = 16;
    memcpy(b->ranges, kept, sizeof(kept[0]) * n);
    b->count = n;
    b->highest = n > 0 ? b->ranges[n - 1].end - 0.5 : NAN;
    fed_result_of(b, out);
}
static void fed_set(void *opaque, void *session, void *buffer, os64_dom_fed_mode_t mode, double offset,
                    double window_start, double window_end)
{
    (void)opaque; (void)session;
    FedBuffer *b = buffer;
    b->mode = mode;
    b->offset = offset;
    b->window_start = window_start;
    b->window_end = window_end;
}
static uint32_t fed_ranges(void *opaque, void *session, void *buffer, os64_dom_fed_range_t *out, uint32_t cap,
                           double *highest_end)
{
    (void)opaque; (void)session;
    FedBuffer *b = buffer;
    uint32_t n = b->count < cap ? b->count : cap;
    memcpy(out, b->ranges, sizeof(out[0]) * n);
    *highest_end = b->count > 0 ? b->ranges[b->count - 1].end : 0;
    return n;
}
static void fed_duration(void *opaque, void *session, double seconds)
{
    (void)opaque;
    ((FedSession *)session)->duration = seconds;
}
static void fed_end(void *opaque, void *session)
{
    (void)opaque;
    ((FedSession *)session)->ends++;
}

static const os64_dom_fed_provider_t fed_provider = {
    NULL, fed_open, fed_close, fed_add, fed_remove_buffer, fed_evict, fed_take, fed_parse, fed_poll, fed_reset,
    fed_remove, fed_set, fed_ranges, fed_duration, fed_end,
};

/* A MediaSource's object URL names it with a random UUID. */
static bool mse_entropy(void *opaque, uint8_t *out, size_t length)
{
    (void)opaque;
    static uint8_t next = 1;
    for (size_t i = 0; i < length; i++) out[i] = next++;
    return true;
}

/* media_fixture's page, with randomness and the fake fed host. */
static DomFixture mse_fixture(DomFixture *slot)
{
    memset(fed_sessions, 0, sizeof(fed_sessions));
    fed_opened = fed_closed = 0;
    fed_refuse_add = false;
    memset(media_sessions, 0, sizeof(media_sessions));
    media_opened = media_closed = 0;
    media_activated = media_refuse = false;
    p2b_ms = 1000;
    p2b_us = 1000000;
    os64_dom_options_t options = p2b_options(P2B_URL);
    options.entropy = mse_entropy;
    *slot = fixture_new(&options);
    if (fixture_ready(slot)) {
        os64_dom_set_media(slot->binding, &media_provider);
        os64_dom_set_fed(slot->binding, &fed_provider);
    }
    return *slot;
}

static int mse_script(DomFixture *f, const char *body, const char *name)
{
    size_t length = strlen(MSE_ASSERT) + strlen(body) + 3;
    char *source = malloc(length);
    if (source == NULL) abort();
    snprintf(source, length, "%s{%s}", MSE_ASSERT, body);
    int result = dom_script(f, source, name);
    free(source);
    return result;
}

/* A video element, a MediaSource attached to it at the turn, and both
 * watched: globals v, ms. */
static void mse_attach(DomFixture *f, const char *what)
{
    mse_script(f, "globalThis.v=document.createElement('video');globalThis.ms=new MediaSource();"
                  "watch(v,'v',V_EVENTS);watch(ms,'ms',MS_EVENTS);watch(ms.sourceBuffers,'list',LIST_EVENTS);"
                  "watch(ms.activeSourceBuffers,'active',LIST_EVENTS);"
                  "globalThis.url=URL.createObjectURL(ms);v.src=url;",
               what);
    media_turn(f, what);
}

static void dom_mse_interface_cases(void)
{
    DomFixture f;
    mse_fixture(&f);
    if (!fixture_ready(&f)) return;
    mse_script(&f,
        "const ms=new MediaSource();"
        "assert(ms instanceof MediaSource&&ms instanceof EventTarget&&String(ms)==='[object MediaSource]');"
        "assert(ms.readyState==='closed'&&isNaN(ms.duration),'closed, no duration');"
        "assert(ms.sourceBuffers instanceof SourceBufferList&&ms.sourceBuffers===ms.sourceBuffers,'one list');"
        "assert(ms.sourceBuffers.length===0&&ms.activeSourceBuffers.length===0&&ms.sourceBuffers[0]===undefined);"
        "assert(ms.sourceBuffers!==ms.activeSourceBuffers,'two lists');"
        "throws('TypeError',()=>new SourceBuffer());throws('TypeError',()=>new SourceBufferList());"
        "assert(MediaSource.canConstructInDedicatedWorker===false);"
        "for(const h of ['onsourceopen','onsourceended','onsourceclose'])assert(h in ms&&ms[h]===null,h);"
        "throws('TypeError',()=>ms.addSourceBuffer(''));"
        "throws('NotSupportedError',()=>ms.addSourceBuffer('video/webm; codecs=\"vp9\"'));"
        "throws('InvalidStateError',()=>ms.addSourceBuffer('video/mp4; codecs=\"avc1.64001f\"'));"
        "throws('InvalidStateError',()=>ms.endOfStream());throws('TypeError',()=>ms.endOfStream('bogus'));"
        "throws('TypeError',()=>{ms.duration=-1});throws('TypeError',()=>{ms.duration=NaN});"
        "throws('InvalidStateError',()=>{ms.duration=5});"
        "throws('InvalidStateError',()=>ms.setLiveSeekableRange(0,1));"
        "throws('InvalidStateError',()=>ms.clearLiveSeekableRange());"
        "const u=URL.createObjectURL(ms);assert(/^blob:https:\\/\\/example\\.test\\/[0-9a-f-]{36}$/.test(u),u);"
        "assert(URL.createObjectURL(ms)!==u,'a new URL each time');URL.revokeObjectURL(u);"
        "throws('TypeError',()=>URL.createObjectURL({}));",
        "mse: the interfaces");
    /* isTypeSupported: H.264 and AAC-LC in MP4, with codecs; nothing else. */
    mse_script(&f,
        "const yes=['video/mp4; codecs=\"avc1.42E01E\"','video/mp4; codecs=\"avc1.4d401f\"',"
        "'video/mp4;codecs=avc1.640033','video/mp4; codecs=\"avc3.64001f\"','audio/mp4; codecs=\"mp4a.40.2\"',"
        "'video/mp4; codecs=\"avc1.64001f, mp4a.40.2\"','VIDEO/MP4; CODECS=\"AVC1.64001F\"',"
        "'video/mp4; codecs=\"mp4a.40.2\"','video/mp4; codecs=\"avc1.64001f\"; profiles=\"isom\"'];"
        "const no=['','video/mp4','audio/mp4','video/mp4; codecs=\"avc1\"','video/mp4; codecs=\"avc1.64001\"',"
        "'video/mp4; codecs=\"avc1.580028\"','video/mp4; codecs=\"avc1.640034\"','audio/mp4; codecs=\"mp4a.40.5\"',"
        "'audio/mp4; codecs=\"mp4a.40.29\"','audio/mp4; codecs=\"avc1.64001f\"','video/webm; codecs=\"vp9\"',"
        "'video/mp4; codecs=\"avc1.64001f, avc1.42E01E\"','video/mp4; codecs=\"hev1.1.6.L93.B0\"',"
        "'video/mp4; codecs=\"\"','video/mp2t; codecs=\"avc1.64001f\"','video/mp4 codecs=\"avc1.64001f\"'];"
        "for(const t of yes)assert(MediaSource.isTypeSupported(t)===true,'yes: '+t);"
        "for(const t of no)assert(MediaSource.isTypeSupported(t)===false,'no: '+t);",
        "mse: isTypeSupported");
    fixture_free(&f);
}

static void dom_mse_attach_cases(void)
{
    DomFixture f;
    mse_fixture(&f);
    if (!fixture_ready(&f)) return;
    mse_attach(&f, "mse: attach");
    check(fed_opened == 1 && fed_sessions[0].open, "mse: the fed session is opened at the turn");
    mse_script(&f, "const h=heard();assert(h==='v:loadstart ms:sourceopen','loadstart then sourceopen: '+h);"
                   "assert(ms.readyState==='open'&&v.networkState===2&&v.readyState===0,'open, loading');",
               "mse: sourceopen");
    /* A second element naming the same source while it is attached fails
     * as a source that cannot be fetched. */
    mse_script(&f, "globalThis.w=document.createElement('video');watch(w,'w',V_EVENTS);w.src=url;", "mse: a second element");
    media_turn(&f, "mse: the second attach is refused");
    mse_script(&f, "const h=heard();assert(h==='w:loadstart w:error','the second fails: '+h);"
                   "assert(w.error.code===4&&ms.readyState==='open','the first stays open');",
               "mse: one attachment at a time");
    check(fed_opened == 1, "mse: no second session");
    /* Revoking the URL leaves the attachment as it is. */
    mse_script(&f, "URL.revokeObjectURL(url);assert(ms.readyState==='open');", "mse: revoke after attach");
    media_turn(&f, "mse: revoke turn");
    mse_script(&f, "assert(ms.readyState==='open'&&heard()==='','still attached');", "mse: revoke keeps it");
    fixture_free(&f);

    /* With no fed host a MediaSource never opens. */
    mse_fixture(&f);
    if (!fixture_ready(&f)) return;
    os64_dom_set_fed(f.binding, NULL);
    mse_attach(&f, "mse: no host");
    mse_script(&f, "const h=heard();assert(h==='v:loadstart v:error'&&ms.readyState==='closed','refused: '+h);",
               "mse: no fed host, no attachment");
    mse_script(&f, "assert(!MediaSource.isTypeSupported('video/mp4; codecs=\"avc1.64001f\"'),"
                   "'with no host nothing is supported, so a player falls back');",
               "mse: no fed host, no type");
    fixture_free(&f);
}

/* An attached source with one SourceBuffer for H.264 (globals sb, ms, v). */
static void mse_with_buffer(DomFixture *f, const char *what)
{
    mse_attach(f, what);
    mse_script(f, "heard();globalThis.sb=ms.addSourceBuffer('video/mp4; codecs=\"avc1.64001f\"');watch(sb,'sb',SB_EVENTS);",
               what);
    media_turn(f, what);
}

static void dom_mse_buffer_cases(void)
{
    DomFixture f;
    mse_fixture(&f);
    if (!fixture_ready(&f)) return;
    mse_with_buffer(&f, "mse: add a buffer");
    FedSession *s = &fed_sessions[0];
    FedBuffer *b = &s->buffers[0];
    check(s->count == 1 && b->live && strcmp(b->type, "video/mp4; codecs=\"avc1.64001f\"") == 0,
          "mse: the host made a buffer for the type");
    mse_script(&f,
        "const h=heard();assert(h==='list:addsourcebuffer','added: '+h);"
        "assert(sb instanceof SourceBuffer&&sb instanceof EventTarget&&ms.sourceBuffers.length===1&&ms.sourceBuffers[0]===sb);"
        "assert([...ms.sourceBuffers][0]===sb&&ms.activeSourceBuffers.length===0,'not active before its init segment');"
        "assert(sb.mode==='segments'&&!sb.updating&&sb.timestampOffset===0&&sb.appendWindowStart===0&&"
        "sb.appendWindowEnd===Infinity&&sb.buffered.length===0,'the start');"
        "throws('TypeError',()=>sb.appendBuffer('text'));"
        "sb.mode='bogus';assert(sb.mode==='segments','an unknown mode is no assignment');"
        "throws('TypeError',()=>{sb.timestampOffset=Infinity});throws('TypeError',()=>{sb.appendWindowStart=-1});"
        "throws('TypeError',()=>{sb.appendWindowEnd=0});throws('NotSupportedError',()=>sb.changeType('video/mp4'));"
        "throws('TypeError',()=>sb.changeType(''));"
        "throws('TypeError',()=>sb.remove(0,1));",
        "mse: a SourceBuffer");
    /* The append: updatestart now, the parse in the task, then update and
     * updateend; the first init segment makes it active and sets the
     * duration, and with every buffer initialized the element has its
     * metadata. */
    mse_script(&f, "sb.appendBuffer(B('init:v:10'));assert(sb.updating,'updating at once');"
                   "throws('InvalidStateError',()=>sb.appendBuffer(B('x')));"
                   "throws('InvalidStateError',()=>{sb.timestampOffset=1});throws('InvalidStateError',()=>{ms.duration=3});"
                   "throws('InvalidStateError',()=>sb.remove(0,1));throws('InvalidStateError',()=>ms.endOfStream());",
               "mse: appendBuffer");
    check(b->takes == 1 && b->evicts == 1 && b->parses == 0, "mse: the bytes are taken at once, parsed in the task");
    media_turn(&f, "mse: the append's task");
    check(b->parses == 1 && s->duration == 10, "mse: parsed; the host told the duration");
    mse_script(&f,
        "const h=heard();assert(h==='sb:updatestart active:addsourcebuffer v:durationchange sb:update sb:updateend "
        "v:loadedmetadata','the order: '+h);"
        "assert(!sb.updating&&ms.activeSourceBuffers[0]===sb&&ms.duration===10&&v.duration===10&&v.readyState===1);",
        "mse: the init segment received");
    mse_script(&f, "sb.appendBuffer(B('frames:0:4'));", "mse: frames");
    media_turn(&f, "mse: frames' task");
    mse_script(&f,
        "assert(heard()==='sb:updatestart sb:update sb:updateend');"
        "assert(R(sb.buffered)==='0-4'&&R(v.buffered)==='0-4'&&sb.buffered===sb.buffered,'one range, one object');"
        "assert(R(v.seekable)==='0-10','seekable is the duration');"
        "throws('InvalidStateError',()=>{ms.duration=3});"
        "ms.duration=3.8;assert(ms.duration===4&&v.duration===4,'raised to the highest end: '+ms.duration);",
        "mse: buffered, seekable, the duration's floor");
    media_turn(&f, "mse: duration turn");
    /* remove(): its argument checks, then the removal as a task. */
    mse_script(&f, "heard();throws('TypeError',()=>sb.remove(NaN,1));throws('TypeError',()=>sb.remove(5,6));"
                   "throws('TypeError',()=>sb.remove(1,1));sb.remove(1,2);assert(sb.updating);"
                   "throws('InvalidStateError',()=>sb.abort());",
               "mse: remove");
    media_turn(&f, "mse: the removal's task");
    check(b->removes == 1, "mse: the host removed");
    mse_script(&f, "assert(heard()==='sb:updatestart sb:update sb:updateend'&&R(sb.buffered)==='0-1,2-4',R(sb.buffered));",
               "mse: removed");
    /* abort() cancels a queued append: abort, updateend, and no parse. */
    mse_script(&f, "sb.appendWindowStart=1;sb.appendBuffer(B('frames:4:6'));sb.abort();"
                   "assert(!sb.updating&&sb.appendWindowStart===0&&sb.appendWindowEnd===Infinity,'the window reset');",
               "mse: abort");
    int parses = b->parses;
    media_turn(&f, "mse: abort's events");
    check(b->parses == parses && b->resets == 1, "mse: the queued parse never ran; the parser was reset");
    mse_script(&f, "const h=heard();assert(h==='sb:updatestart sb:abort sb:updateend','aborted: '+h);"
                   "assert(R(sb.buffered)==='0-1,2-4');",
               "mse: aborted");
    fixture_free(&f);
}

static void dom_mse_sequence_cases(void)
{
    DomFixture f;
    mse_fixture(&f);
    if (!fixture_ready(&f)) return;
    mse_with_buffer(&f, "mse: sequence");
    /* In sequence mode the coded frame processing moves timestampOffset,
     * and the attribute shows it. */
    mse_script(&f, "heard();sb.appendBuffer(B('init:v:nan'));", "mse: sequence init");
    media_turn(&f, "mse: sequence init turn");
    mse_script(&f, "assert(ms.duration===Infinity,'an init segment with no duration: '+ms.duration);"
                   "sb.mode='sequence';sb.appendBuffer(B('frames:5:7'));",
               "mse: sequence frames");
    media_turn(&f, "mse: sequence frames turn");
    mse_script(&f, "assert(sb.timestampOffset===-5,'moved: '+sb.timestampOffset);sb.appendBuffer(B('frames:5:7'));",
               "mse: the offset moved");
    media_turn(&f, "mse: sequence again");
    mse_script(&f, "assert(sb.timestampOffset===-3&&R(sb.buffered)==='0-4',sb.timestampOffset+' '+R(sb.buffered));"
                   "assert(R(v.seekable)==='0-4','an infinite duration seeks the buffered');"
                   "ms.setLiveSeekableRange(1,9);assert(R(v.seekable)==='0-9',R(v.seekable));"
                   "ms.clearLiveSeekableRange();assert(R(v.seekable)==='0-4');",
               "mse: the offset moved again; live seekable");
    /* Inside a media segment the mode and offset cannot change. */
    mse_script(&f, "sb.appendBuffer(B('media'));", "mse: into a media segment");
    media_turn(&f, "mse: media turn");
    mse_script(&f, "throws('InvalidStateError',()=>{sb.timestampOffset=1});throws('InvalidStateError',()=>{sb.mode='segments'});"
                   "sb.abort();sb.timestampOffset=1;assert(sb.timestampOffset===1,'abort resets the parser');",
               "mse: PARSING_MEDIA_SEGMENT");
    /* A parse elsewhere: updating until the poll answers. */
    mse_script(&f, "heard();sb.appendBuffer(B('pending:2 frames:0:1'));", "mse: pending");
    media_turn(&f, "mse: pending turn 1");
    mse_script(&f, "assert(sb.updating&&heard()==='sb:updatestart','still parsing');", "mse: still parsing");
    media_turn(&f, "mse: pending turn 2");
    mse_script(&f, "assert(!sb.updating&&heard()==='sb:update sb:updateend','the poll ended it');", "mse: parsed elsewhere");
    fixture_free(&f);
}

static void dom_mse_end_cases(void)
{
    DomFixture f;
    mse_fixture(&f);
    if (!fixture_ready(&f)) return;
    mse_with_buffer(&f, "mse: end");
    mse_script(&f, "heard();sb.appendBuffer(B('init:v:10 frames:0:4'));", "mse: end setup");
    media_turn(&f, "mse: end setup turn");
    /* endOfStream(): ended, the duration to the highest end, and the last
     * range reaching it; an append reopens. */
    mse_script(&f, "heard();ms.endOfStream();assert(ms.readyState==='ended'&&ms.duration===4,ms.duration);"
                   "throws('InvalidStateError',()=>sb.abort());",
               "mse: endOfStream");
    media_turn(&f, "mse: endOfStream turn");
    check(fed_sessions[0].ends == 1, "mse: the host heard the end");
    mse_script(&f, "const h=heard();assert(h==='ms:sourceended v:durationchange','ended: '+h);"
                   "sb.appendBuffer(B('frames:4:5'));assert(ms.readyState==='open');",
               "mse: sourceended; an append reopens");
    media_turn(&f, "mse: reopen turn");
    mse_script(&f, "const h=heard();assert(h==='ms:sourceopen sb:updatestart v:durationchange sb:update sb:updateend',h);"
                   "assert(ms.duration===5,'past the group end: '+ms.duration);",
               "mse: reopened");
    /* The append error: error, updateend, then the end of stream with a
     * decode error; after metadata, the element's MEDIA_ERR_DECODE. */
    mse_script(&f, "sb.appendBuffer(B('error'));", "mse: a broken segment");
    media_turn(&f, "mse: the append error");
    mse_script(&f, "const h=heard();assert(h==='sb:updatestart sb:error sb:updateend ms:sourceended v:error','error: '+h);"
                   "assert(ms.readyState==='ended'&&v.error.code===3,'decode error: '+(v.error&&v.error.code));"
                   "throws('InvalidStateError',()=>sb.appendBuffer(B('frames:5:6')));",
               "mse: append error");
    fixture_free(&f);

    /* An append error before metadata is a source that cannot be decoded. */
    mse_fixture(&f);
    if (!fixture_ready(&f)) return;
    mse_with_buffer(&f, "mse: early error");
    mse_script(&f, "heard();sb.appendBuffer(B('error'));", "mse: early broken segment");
    media_turn(&f, "mse: early error turn");
    mse_script(&f, "const h=heard();assert(h==='sb:updatestart sb:error sb:updateend ms:sourceended v:error',h);"
                   "assert(v.error.code===4&&v.networkState===3,'not supported');",
               "mse: append error with nothing yet");
    fixture_free(&f);

    /* A decode error in play, long after its append succeeded, ends the
     * source with a decode error (the session's facts.error). */
    mse_fixture(&f);
    if (!fixture_ready(&f)) return;
    mse_with_buffer(&f, "mse: decode in play");
    mse_script(&f, "heard();sb.appendBuffer(B('init:v:10 frames:0:4'));", "mse: decode setup");
    media_turn(&f, "mse: decode setup turn");
    mse_script(&f, "heard();", "mse: decode clear");
    fed_sessions[0].media.facts.error = OS64_DOM_MEDIA_ERR_DECODE;
    media_turn(&f, "mse: the decoder refuses");
    mse_script(&f, "const h=heard();assert(h==='ms:sourceended v:error','decode in play: '+h);"
                   "assert(ms.readyState==='ended'&&v.error.code===3);",
               "mse: a decode error in play ends the stream");
    fixture_free(&f);

    /* An append no SourceBuffer can ever hold is QuotaExceededError, at
     * once, with nothing taken or queued. */
    mse_fixture(&f);
    if (!fixture_ready(&f)) return;
    mse_with_buffer(&f, "mse: oversize");
    mse_script(&f, "heard();throws('QuotaExceededError',()=>sb.appendBuffer(new Uint8Array(2*1024*1024)));"
                   "assert(!sb.updating);",
               "mse: an oversize append");
    media_turn(&f, "mse: oversize turn");
    check(fed_sessions[0].buffers[0].takes == 0, "mse: nothing taken");
    mse_script(&f, "assert(heard()==='','no events');", "mse: no events");
    fixture_free(&f);
}

static void dom_mse_remove_cases(void)
{
    DomFixture f;
    mse_fixture(&f);
    if (!fixture_ready(&f)) return;
    /* removeSourceBuffer while updating: abort and updateend at the
     * buffer, then removesourcebuffer at each list it was in. */
    mse_with_buffer(&f, "mse: removeSourceBuffer");
    mse_script(&f, "heard();sb.appendBuffer(B('init:v:10'));", "mse: rsb init");
    media_turn(&f, "mse: rsb init turn");
    mse_script(&f, "heard();sb.appendBuffer(B('frames:0:4'));"
                   "throws('TypeError',()=>ms.removeSourceBuffer({}));"
                   "ms.removeSourceBuffer(sb);assert(ms.sourceBuffers.length===0&&ms.activeSourceBuffers.length===0);"
                   "throws('InvalidStateError',()=>sb.appendBuffer(B('x')));throws('InvalidStateError',()=>sb.abort());"
                   "throws('InvalidStateError',()=>sb.buffered);throws('NotFoundError',()=>ms.removeSourceBuffer(sb));",
               "mse: removeSourceBuffer while updating");
    int parses = fed_sessions[0].buffers[0].parses;
    media_turn(&f, "mse: rsb turn");
    check(!fed_sessions[0].buffers[0].live && fed_sessions[0].buffers[0].parses == parses,
          "mse: the host's buffer is gone and the queued parse never ran");
    mse_script(&f, "const h=heard();assert(h==='sb:updatestart sb:abort sb:updateend active:removesourcebuffer "
                   "list:removesourcebuffer',h);",
               "mse: removed while updating");
    fixture_free(&f);
}

static void dom_mse_detach_cases(void)
{
    DomFixture f;
    mse_fixture(&f);
    if (!fixture_ready(&f)) return;
    /* A detach with an append in flight: abort and updateend at the buffer,
     * then the lists, then sourceclose (both other browsers). */
    mse_with_buffer(&f, "mse: detach");
    mse_script(&f, "heard();sb.appendBuffer(B('init:v:10'));", "mse: detach init");
    media_turn(&f, "mse: detach init turn");
    mse_script(&f, "heard();sb.appendBuffer(B('frames:0:4'));v.removeAttribute('src');v.load();"
                   "assert(ms.readyState==='closed'&&isNaN(ms.duration)&&ms.sourceBuffers.length===0&&!sb.updating);",
               "mse: detach with an append in flight");
    check(fed_closed == 1 && !fed_sessions[0].open, "mse: the fed session is closed");
    media_turn(&f, "mse: detach events");
    mse_script(&f, "const h=heard();assert(h==='sb:updatestart sb:abort sb:updateend active:removesourcebuffer "
                   "list:removesourcebuffer ms:sourceclose v:emptied',h);"
                   "throws('InvalidStateError',()=>sb.appendBuffer(B('x')));",
               "mse: the detach's events");
    /* Detached, it may attach again. */
    mse_script(&f, "v.src=URL.createObjectURL(ms);", "mse: reattach");
    media_turn(&f, "mse: reattach turn");
    mse_script(&f, "const h=heard();assert(ms.readyState==='open'&&h==='v:loadstart ms:sourceopen',h);",
               "mse: attached again");
    check(fed_opened == 2, "mse: a new session");
    fixture_free(&f);

    /* A detach with a removal in flight cancels it with no events. */
    mse_fixture(&f);
    if (!fixture_ready(&f)) return;
    mse_with_buffer(&f, "mse: detach removal");
    mse_script(&f, "heard();sb.appendBuffer(B('init:v:10 frames:0:4'));", "mse: dr setup");
    media_turn(&f, "mse: dr setup turn");
    mse_script(&f, "heard();sb.remove(0,1);v.removeAttribute('src');v.load();", "mse: detach with a removal");
    media_turn(&f, "mse: dr events");
    check(fed_sessions[0].buffers[0].removes == 0, "mse: the removal never ran");
    mse_script(&f, "const h=heard();assert(h==='sb:updatestart active:removesourcebuffer list:removesourcebuffer "
                   "ms:sourceclose v:emptied',h);",
               "mse: no abort for a removal");
    fixture_free(&f);
}

/* A queued task holds its target: objects the page dropped still hear
 * their events. */
static void dom_mse_lifetime_cases(void)
{
    DomFixture f;
    mse_fixture(&f);
    if (!fixture_ready(&f)) return;
    mse_script(&f, "globalThis.v=document.createElement('video');"
                   "(()=>{const ms=new MediaSource();watch(ms,'ms',MS_EVENTS);globalThis.u=URL.createObjectURL(ms);"
                   "v.src=u;})();",
               "mse: a source the page holds only by its URL");
    media_turn(&f, "mse: lifetime attach");
    mse_script(&f, "URL.revokeObjectURL(u);assert(heard()==='ms:sourceopen');", "mse: the URL revoked");
    /* The element holds the attached source; after the detach only the
     * queued sourceclose holds it. */
    mse_script(&f, "v.removeAttribute('src');v.load();",
               "mse: detach the dropped source");
    os64_js_outcome_t gc;
    JS_RunGC(JS_GetRuntime(os64_js_context(f.runtime, OS64_JS_ABI_ID, &gc)));
    media_turn(&f, "mse: the queued sourceclose");
    mse_script(&f, "const h=heard();assert(h==='ms:sourceclose','still heard: '+h);", "mse: the task held its target");

    /* A SourceBuffer removed with its append queued: only the queue holds
     * it, and it hears abort and updateend. */
    mse_script(&f, "globalThis.ms2=new MediaSource();v.src=URL.createObjectURL(ms2);", "mse: lifetime buffer");
    media_turn(&f, "mse: lifetime buffer attach");
    mse_script(&f, "heard();(()=>{const sb=ms2.addSourceBuffer('video/mp4; codecs=\"avc1.64001f\"');watch(sb,'sb',SB_EVENTS);"
                   "sb.appendBuffer(B('init:v:1'));ms2.removeSourceBuffer(sb);})();",
               "mse: drop a buffer with its append queued");
    JS_RunGC(JS_GetRuntime(os64_js_context(f.runtime, OS64_JS_ABI_ID, &gc)));
    media_turn(&f, "mse: the dropped buffer's events");
    mse_script(&f, "const h=heard();assert(h==='sb:updatestart sb:abort sb:updateend','still heard: '+h);",
               "mse: the queue held the buffer");
    fixture_free(&f);
}

static void dom_mse_two_buffer_cases(void)
{
    DomFixture f;
    mse_fixture(&f);
    if (!fixture_ready(&f)) return;
    mse_attach(&f, "mse: two buffers");
    mse_script(&f,
        "heard();globalThis.a=ms.addSourceBuffer('audio/mp4; codecs=\"mp4a.40.2\"');"
        "globalThis.b=ms.addSourceBuffer('video/mp4; codecs=\"avc1.64001f\"');"
        "throws('QuotaExceededError',()=>ms.addSourceBuffer('video/mp4; codecs=\"avc1.64001f\"'));"
        "a.appendBuffer(B('init:a:10'));",
        "mse: two buffers");
    media_turn(&f, "mse: the first init");
    mse_script(&f, "assert(v.readyState===0,'metadata waits for every buffer: '+heard());"
                   "b.appendBuffer(B('init:v:10'));",
               "mse: one initialized");
    media_turn(&f, "mse: the second init");
    mse_script(&f, "assert(v.readyState===1&&ms.activeSourceBuffers.length===2,'both: '+v.readyState);"
                   "a.appendBuffer(B('frames:0:4 frames:5:8'));",
               "mse: metadata");
    media_turn(&f, "mse: audio frames");
    mse_script(&f, "b.appendBuffer(B('frames:1:6'));", "mse: video frames");
    media_turn(&f, "mse: video frames turn");
    mse_script(&f, "assert(R(v.buffered)==='1-4,5-6','the intersection: '+R(v.buffered));"
                   "ms.endOfStream();assert(R(v.buffered)==='1-4,5-8','ended: each reaches the highest end: '+R(v.buffered));",
               "mse: the element's buffered");
    fixture_free(&f);

    /* A host that cannot make a buffer: QuotaExceededError. */
    mse_fixture(&f);
    if (!fixture_ready(&f)) return;
    mse_attach(&f, "mse: refused buffer");
    fed_refuse_add = true;
    mse_script(&f, "throws('QuotaExceededError',()=>ms.addSourceBuffer('video/mp4; codecs=\"avc1.64001f\"'));"
                   "assert(ms.sourceBuffers.length===0);",
               "mse: the host refuses a buffer");
    fixture_free(&f);
}

static void dom_mse_quality_cases(void)
{
    DomFixture f;
    mse_fixture(&f);
    if (!fixture_ready(&f)) return;
    mse_with_buffer(&f, "mse: quality");
    fed_sessions[0].media.facts.frames_shown = 10;
    fed_sessions[0].media.facts.frames_dropped = 2;
    mse_script(&f, "const q=v.getVideoPlaybackQuality();assert(q instanceof VideoPlaybackQuality);"
                   "assert(q.totalVideoFrames===12&&q.droppedVideoFrames===2&&q.corruptedVideoFrames===0&&"
                   "typeof q.creationTime==='number','counts: '+q.totalVideoFrames+'/'+q.droppedVideoFrames);"
                   "const w=document.createElement('video').getVideoPlaybackQuality();"
                   "assert(w.totalVideoFrames===0,'no session, no frames');"
                   "throws('TypeError',()=>new VideoPlaybackQuality());",
               "mse: getVideoPlaybackQuality");
    fixture_free(&f);
}

static void dom_mse_cases(void)
{
    dom_mse_interface_cases();
    dom_mse_attach_cases();
    dom_mse_buffer_cases();
    dom_mse_sequence_cases();
    dom_mse_end_cases();
    dom_mse_remove_cases();
    dom_mse_detach_cases();
    dom_mse_lifetime_cases();
    dom_mse_two_buffer_cases();
    dom_mse_quality_cases();
}
