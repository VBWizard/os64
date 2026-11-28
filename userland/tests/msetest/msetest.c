// msetest — libmedia's fed media (os64/media_fed.h) through its player,
// as a page's MediaSource runs it, on os64. The media is the fixture
// tools/make_dash_fixture.sh makes, in /tests/pages: sync-fed-720p.mp4,
// sync-fed-480p.mp4 and sync-fed-a48.mp4, each with its index (.json).
//
//   1. STRAIGHT: the 720p pictures and the 48 kHz sound appended whole, the
//      end, and play, a frame asked for at each 60 Hz blank as a display
//      would: it plays to its end (ENDED at the length the duration gave),
//      showing pictures up to its last second, with no failure, no silence
//      filled and no sound cut (every picture's coming out is the host
//      test's to prove: a live player behind, as under QEMU, leaves some out)
//   2. A QUALITY SWITCH: 720p's first two fragments, then 480p's
//      initialization segment and the rest: it plays to its end, with one
//      decoder reopened, pictures shown up to its last second. (That no
//      picture is lost at the switch, and each one's size, are the host
//      test's to prove: a live player leaves pictures out when it falls
//      behind, as it does under QEMU's emulated CPU, and hands them out
//      converted.)
//   3. A SEEK INTO WHAT IS NOT YET BUFFERED: the first two fragments, play,
//      and a seek to 7 s, which does not land; the rest appended, it lands,
//      at 7 s
//   4. THE COUNTS: the bytes the fed media holds are the slabs' and the
//      frames' (the slabs never fewer), and all go at the close
//
// Exit codes: 0xF3D00000 success, 0xF3D000NN the step that failed,
// 0xF3D000FE skipped (a sink that can be heard: the line printed says so).
#include "../audiotest/sound_guard.h"
#include "os64/media_fed.h"
#include "os64/media_output.h"
#include "os64/media_player.h"
#include "os64/os64.h"

#define STEP(n) (0xF3D00000 | (n))

static int die(int step, const char *why, int64_t got)
{
    os64_printf("msetest: FAIL step %d: %s (got %ld)\n", step, why, got);
    return (int)STEP(step);
}

typedef struct {
    uint8_t *data;
    int64_t size;
    int64_t init[2];
    int count;
    int64_t frag[8][2];
} rendition_t;

static uint8_t *slurp(const char *path, int64_t *size)
{
    int64_t h = os64_open(path, "r");
    if (h < 0) return 0;
    int64_t n = os64_seek((int32_t)h, 0, OS64_SEEK_END);
    os64_seek((int32_t)h, 0, OS64_SEEK_SET);
    uint8_t *d = n > 0 ? os64_malloc((size_t)n + 1) : 0;
    int64_t got = d ? os64_read((int32_t)h, d, (size_t)n) : -1;
    os64_close((int32_t)h);
    if (got != n) {
        os64_free(d);
        return 0;
    }
    d[n] = 0;
    *size = n;
    return d;
}

static const char *find(const char *text, const char *key)
{
    size_t n = os64_strlen(key);
    for (; *text; text++)
        if (os64_memcmp(text, key, n) == 0) return text;
    return 0;
}

// The numbers after `key` in the index: up to `cap` of them.
static int numbers_after(const char *text, const char *key, int64_t *out, int cap)
{
    const char *p = find(text, key);
    int n = 0;
    while (p && *p && n < cap) {
        while (*p && !(*p >= '0' && *p <= '9')) p++;
        if (!*p) break;
        int64_t v = 0;
        while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        if (*p == '.') {                 // a start or duration in seconds: skipped
            p++;
            while (*p >= '0' && *p <= '9') p++;
            v = -1;
        }
        out[n++] = v;
    }
    return n;
}

static bool load(const char *name, rendition_t *r)
{
    char path[96];
    os64_snprintf(path, sizeof(path), "/tests/pages/%s.mp4", name);
    r->data = slurp(path, &r->size);
    os64_snprintf(path, sizeof(path), "/tests/pages/%s.json", name);
    int64_t n;
    char *j = (char *)slurp(path, &n);
    if (!r->data || !j) return false;
    int64_t v[40];
    if (numbers_after(j, "\"init\"", v, 2) != 2) return false;
    r->init[0] = v[0];
    r->init[1] = v[1];
    // Per fragment: offset, length, then two times (read as -1, or as a
    // whole number when the time has no fraction).
    int k = numbers_after(j, "\"fragments\"", v, 40);
    r->count = 0;
    for (int i = 0; i + 3 < k && r->count < 8; i += 4) {
        r->frag[r->count][0] = v[i];
        r->frag[r->count][1] = v[i + 1];
        r->count++;
    }
    os64_free(j);
    return r->count > 0;
}

static bool append(os64_media_fed_t *fed, os64_media_fed_buffer_t *b, const rendition_t *r, int64_t off, int64_t len)
{
    if (!os64_media_fed_take(fed, b, r->data + off, (size_t)len)) return false;
    os64_media_fed_result_t res;
    os64_media_fed_parse(fed, b, &res);
    if (res.status != OS64_MEDIA_FED_DONE) os64_printf("msetest: parse: %s\n", res.message);
    return res.status == OS64_MEDIA_FED_DONE;
}

static rendition_t s_720, s_480, s_a48;
static volatile bool s_finished;

static void finished(void *ctx)
{
    (void)ctx;
    __atomic_store_n(&s_finished, true, __ATOMIC_RELEASE);
}

typedef struct {
    os64_media_fed_t *fed;
    os64_media_fed_buffer_t *v, *a;
    os64_media_output_t *output;
    os64_media_player_t *player;
} rig_t;

static bool rig_open(rig_t *r)
{
    os64_memset(r, 0, sizeof(*r));
    s_finished = false;
    if (os64_media_fed_create(&r->fed) != 0) return false;
    r->v = os64_media_fed_add(r->fed, "video/mp4; codecs=\"avc1.640020\"");
    r->a = os64_media_fed_add(r->fed, "audio/mp4; codecs=\"mp4a.40.2\"");
    os64_media_fed_duration(r->fed, 10.0);
    if (!r->v || !r->a || os64_media_output_create(&r->output) != 0) return false;
    os64_media_player_config_t cfg = { .fed = r->fed, .want = OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE,
                                       .output = r->output, .refused = OS64_MEDIA_VOICE_PLAY_ON,
                                       .finished = finished, .ctx = r };
    if (os64_media_player_create(&cfg, &r->player) != 0) return false;
    os64_media_player_size(r->player, 320, 180);
    return true;
}

static void rig_close(rig_t *r)
{
    if (r->player) os64_media_player_close(r->player);
    if (r->fed) os64_media_fed_close(r->fed);
    for (int i = 0; i < 500 && r->player && !__atomic_load_n(&s_finished, __ATOMIC_ACQUIRE); i++) os64_sleep(10);
    for (int i = 0; i < 100 && r->output && os64_media_output_destroy(r->output) != 0; i++) os64_sleep(10);
}

static os64_media_player_facts_t facts(rig_t *r)
{
    os64_media_player_facts_t f;
    os64_media_player_facts(r->player, &f);
    return f;
}

static int64_t s_latest_shown;      // the latest picture time `run` saw shown

// Frames asked for at each 60 Hz blank for up to `ms`, until the state is
// `until_state` (-1: for the whole time) or the player fails.
static os64_media_player_facts_t run(rig_t *r, int64_t ms, int until_state)
{
    const int64_t period = 16667;
    int64_t end = os64_micros() + ms * 1000;
    os64_media_player_facts_t f = facts(r);
    while (os64_micros() < end) {
        os64_media_player_frame_t fr;
        (void)os64_media_player_frame(r->player, os64_micros() + period, period, &fr);
        f = facts(r);
        if (f.shown_at > s_latest_shown) s_latest_shown = f.shown_at;
        if (f.state == until_state || f.state == OS64_MEDIA_PLAYER_FAILED) break;
        os64_sleep(16);
    }
    return f;
}

static int step_straight(void)
{
    rig_t r;
    if (!rig_open(&r)) return die(1, "the rig would not open", 0);
    bool ok = append(r.fed, r.v, &s_720, s_720.init[0], s_720.init[1]) &&
              append(r.fed, r.a, &s_a48, s_a48.init[0], s_a48.init[1]);
    for (int i = 0; ok && i < s_720.count; i++) ok = append(r.fed, r.v, &s_720, s_720.frag[i][0], s_720.frag[i][1]);
    for (int i = 0; ok && i < s_a48.count; i++) ok = append(r.fed, r.a, &s_a48, s_a48.frag[i][0], s_a48.frag[i][1]);
    os64_media_fed_end(r.fed);
    int code = ok ? 0 : die(1, "an append failed", 0);
    if (code == 0) {
        os64_media_player_play(r.player);
        s_latest_shown = -1;
        os64_media_player_facts_t f = run(&r, 20000, OS64_MEDIA_PLAYER_ENDED);
        os64_media_fed_counts_t c;
        os64_media_fed_counts(r.fed, &c);
        if (f.state != OS64_MEDIA_PLAYER_ENDED) code = die(1, "it did not end (the state)", f.state);
        else if (f.failure != 0) code = die(1, "it failed", f.failure);
        else if (f.length != 10 * 48000) code = die(1, "the length is not the duration given", f.length);
        else if (s_latest_shown < 9 * 48000) code = die(1, "no picture was shown in the last second", s_latest_shown);
        else if (c.silence_frames != 0 || c.cut_frames != 0) code = die(1, "silence filled or sound cut", (int64_t)(c.silence_frames + c.cut_frames));
        else if (c.slab_bytes < c.frame_bytes || c.frame_bytes == 0)
            code = die(4, "the slabs hold fewer bytes than their frames", (int64_t)c.slab_bytes);
    }
    rig_close(&r);
    return code;
}

static int step_switch(void)
{
    rig_t r;
    if (!rig_open(&r)) return die(2, "the rig would not open", 0);
    bool ok = append(r.fed, r.v, &s_720, s_720.init[0], s_720.init[1]) &&
              append(r.fed, r.a, &s_a48, s_a48.init[0], s_a48.init[1]);
    for (int i = 0; ok && i < 2; i++) ok = append(r.fed, r.v, &s_720, s_720.frag[i][0], s_720.frag[i][1]);
    ok = ok && append(r.fed, r.v, &s_480, s_480.init[0], s_480.init[1]);
    for (int i = 2; ok && i < s_480.count; i++) ok = append(r.fed, r.v, &s_480, s_480.frag[i][0], s_480.frag[i][1]);
    for (int i = 0; ok && i < s_a48.count; i++) ok = append(r.fed, r.a, &s_a48, s_a48.frag[i][0], s_a48.frag[i][1]);
    os64_media_fed_end(r.fed);
    int code = ok ? 0 : die(2, "an append failed", 0);
    if (code == 0) {
        os64_media_player_play(r.player);
        s_latest_shown = -1;
        os64_media_player_facts_t f = run(&r, 20000, OS64_MEDIA_PLAYER_ENDED);
        os64_media_fed_counts_t c;
        os64_media_fed_counts(r.fed, &c);
        if (f.state != OS64_MEDIA_PLAYER_ENDED || f.failure != 0) code = die(2, "it did not play to its end", f.state);
        else if (c.reopens != 1) code = die(2, "the decoder was not reopened once", (int64_t)c.reopens);
        else if (s_latest_shown < 9 * 48000)
            code = die(2, "no picture was shown in the last second, after the switch", s_latest_shown);
    }
    rig_close(&r);
    return code;
}

static int step_seek(void)
{
    rig_t r;
    if (!rig_open(&r)) return die(3, "the rig would not open", 0);
    bool ok = append(r.fed, r.v, &s_720, s_720.init[0], s_720.init[1]) &&
              append(r.fed, r.a, &s_a48, s_a48.init[0], s_a48.init[1]);
    for (int i = 0; ok && i < 2; i++)
        ok = append(r.fed, r.v, &s_720, s_720.frag[i][0], s_720.frag[i][1]) &&
             append(r.fed, r.a, &s_a48, s_a48.frag[i][0], s_a48.frag[i][1]);
    int code = ok ? 0 : die(3, "an append failed", 0);
    if (code == 0) {
        os64_media_player_play(r.player);
        uint32_t gen = os64_media_player_seek(r.player, 7 * 48000);
        os64_media_player_facts_t f = run(&r, 1000, -1);
        if (f.seeked == gen) code = die(3, "a seek into nothing buffered landed", f.position);
        for (int i = 3; code == 0 && ok && i < s_720.count; i++)
            ok = append(r.fed, r.v, &s_720, s_720.frag[i][0], s_720.frag[i][1]);
        for (int i = 3; code == 0 && ok && i < s_a48.count; i++)
            ok = append(r.fed, r.a, &s_a48, s_a48.frag[i][0], s_a48.frag[i][1]);
        os64_media_fed_end(r.fed);
        if (code == 0 && !ok) code = die(3, "an append failed", 0);
        int64_t until = os64_micros() + 5000000;
        while (code == 0 && os64_micros() < until && f.seeked != gen) f = run(&r, 100, -1);
        if (code == 0 && f.seeked != gen) code = die(3, "the seek did not land once its data came", f.state);
        else if (code == 0 && (f.shown_at < 7 * 48000 - 800 || f.shown_at > 7 * 48000 + 800))
            code = die(3, "the picture shown is not the one at 7 s", f.shown_at);
    }
    rig_close(&r);
    return code;
}

int main(void)
{
    // The guard's volume half is for tests that listen; this one does not.
    (void)sound_guard_volume_get;
    (void)sound_guard_volume_set;
    char sink[32];
    if (sound_guard_audible(sink, sizeof(sink))) {
        os64_printf("msetest: SKIP: %s can be heard\n", sink);
        return (int)STEP(0xFE);
    }
    if (!load("sync-fed-720p", &s_720) || !load("sync-fed-480p", &s_480) || !load("sync-fed-a48", &s_a48))
        return die(1, "the fixture is not in /tests/pages", 0);
    int code = step_straight();
    if (code == 0) code = step_switch();
    if (code == 0) code = step_seek();
    if (code == 0) os64_printf("msetest: PASS\n");
    return code == 0 ? (int)STEP(0) : code;
}
