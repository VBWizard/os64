// mediakeystest — the media keys (os64/mediakeys.h, MEDIA_KEYS.md) through
// the real syscalls, with presses written to /sys/mediakeys as the
// headphones would send them. Each step has its own exit code:
//
//    1. /dev/mediakeys opens "u" and nothing else; a read shorter than a
//       record is refused; a state is one word, refused otherwise; the
//       handle is listed as `mediakeys`;
//    2. the rule: the newest session to say `playing` takes a press; with
//       none playing, the most recently active; Play and Pause are given
//       as the target's toggle; a record carries its serial and when; a
//       write whose middle line is refused still carries out the others,
//       and answers refused;
//    3. a session whose ring is full is passed over, and the report says
//       so; its eight presses are still there, in order;
//    4. a second reader on a parked session is refused BUSY, and a press
//       wakes the parked one promptly;
//    5. a signal the program handles ends a parked read with
//       OS64_INTERRUPTED;
//    6. a close under a parked read ends it with 0, promptly, and a closed
//       session takes no press;
//    7. with no session, Play goes to the kernel, which has nothing to
//       pause or resume and says so;
//    8. a killed program's session ends with it;
//    9. sessions past the machine's are refused FULL.
//
// It runs only when no other session is open and nothing is playing: a
// press it writes would reach another program's session, or pause somebody's
// sound. Otherwise it says why and reports SKIP.
//
// `mediakeystest` runs it; `mediakeystest poke <task>` and `mediakeystest
// hold` are the helpers it spawns.
//
// Exit codes: 0x3E4B0000 success, 0x3E4B00NN the step that failed,
// 0x3E4B00FE skipped.

#include "os64/os64.h"
#include "os64/audio.h"
#include "os64/mediakeys.h"
#include "os64/signal.h"

#define MKTEST_OK   0x3E4B0000
#define MKTEST_SKIP 0x3E4B00FE
#define STEP(n)     (0x3E4B0000 | (n))

#define SETTLE_MS  200       // enough for a thread to be parked before the main thread acts
#define PROMPT_US  400000    // a wake that took longer waited for the kernel's backstop
#define RING       8         // a session's unread presses
#define SESSIONS   32        // the machine's sessions

static int die(int step, const char *why, int64_t got)
{
    os64_printf("mediakeystest: FAIL step %d: %s (got %ld)\n", step, why, got);
    return (int)STEP(step);
}

// ── /sys/mediakeys ──────────────────────────────────────────────────────

static char gReport[8192];

// Where `needle` first appears in `hay`, or NULL.
static const char *find(const char *hay, const char *needle)
{
    size_t n = os64_strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] == needle[i])
            i++;
        if (i == n)
            return hay;
    }
    return NULL;
}

static bool report(void)
{
    int64_t h = os64_open("/sys/mediakeys", "r");
    if (h < 0)
        return false;
    int64_t n = os64_read((int32_t)h, gReport, sizeof(gReport) - 1);
    os64_close((int32_t)h);
    gReport[n > 0 ? n : 0] = 0;
    return n > 0;
}

static bool report_has(const char *text)
{
    return report() && find(gReport, text) != NULL;
}

static uint64_t parse_number(const char *s)
{
    uint64_t n = 0;
    for (; *s >= '0' && *s <= '9'; s++)
        n = n * 10 + (uint64_t)(*s - '0');
    return n;
}

// The number after `key` in the report, or -1.
static int64_t report_number(const char *key)
{
    if (!report())
        return -1;
    const char *at = find(gReport, key);
    return at != NULL ? (int64_t)parse_number(at + os64_strlen(key)) : -1;
}

static bool press(const char *key)
{
    char text[32];
    int len = os64_snprintf(text, sizeof(text), "press %s\n", key);
    int64_t h = os64_open("/sys/mediakeys", "w");
    if (h < 0)
        return false;
    int64_t wrote = os64_write((int32_t)h, text, (size_t)len);
    os64_close((int32_t)h);
    return wrote == len;
}

// ── Sessions ────────────────────────────────────────────────────────────

static bool say(int32_t s, const char *state)
{
    return os64_write(s, state, os64_strlen(state)) == (int64_t)os64_strlen(state);
}

// The next press, waiting up to `ms`: its key, or the read's answer when
// negative (or 0 for none at all).
static int64_t take(int32_t s, uint64_t ms, os64_media_key_t *out)
{
    os64_media_key_t k;
    int64_t n = os64_read_for(s, &k, sizeof(k), ms);
    if (n != (int64_t)sizeof(k))
        return n < 0 ? n : 0;
    if (out != NULL)
        *out = k;
    return k.key;
}

static bool nothing_waiting(int32_t s)
{
    return take(s, 0, NULL) == OS64_ERR_TIMEOUT;
}

static int64_t read_forever(void *arg)
{
    os64_media_key_t k;
    int64_t n = os64_read(*(int32_t *)arg, &k, sizeof(k));
    return n == (int64_t)sizeof(k) ? (int64_t)k.key : n;
}

static volatile int gHandled;
static void on_interrupt(int signo) { gHandled = signo; }

// ── Helpers spawned ─────────────────────────────────────────────────────

static int poke_main(const char *task_text)
{
    char path[64];
    os64_snprintf(path, sizeof(path), "/proc/%lu/ctl", (unsigned long)parse_number(task_text));
    os64_sleep(SETTLE_MS);              // let the parent park first
    int64_t ctl = os64_open(path, "w");
    if (ctl < 0)
        return 1;
    int64_t wrote = os64_write((int32_t)ctl, "interrupt", 9);
    os64_close((int32_t)ctl);
    return wrote == 9 ? 0 : 2;
}

// Opens a session, says playing, and waits to be killed.
static int hold_main(void)
{
    int64_t s = os64_open("/dev/mediakeys", "u");
    if (s < 0 || !say((int32_t)s, "playing"))
        return 1;
    for (;;)
        os64_sleep(1000);
}

// ── The test ────────────────────────────────────────────────────────────

static bool stream_idle(void)
{
    int64_t h = os64_open("/dev/audiopos", "r");
    if (h < 0)
        return false;
    os64_audio_position_t p;
    int64_t n = os64_read((int32_t)h, &p, sizeof(p));
    os64_close((int32_t)h);
    return n == (int64_t)sizeof(p) && p.state == OS64_AUDIO_IDLE;
}

static int run(void)
{
    if (!report())
        return die(1, "/sys/mediakeys would not read", 0);
    if (report_number("sessions: ") != 0 || !stream_idle()) {
        os64_printf("mediakeystest: SKIP: another program holds a media-key session or the sound is busy\n");
        return MKTEST_SKIP;
    }

    // 1. The handle.
    int64_t r = os64_open("/dev/mediakeys", "r");
    if (r >= 0)
        return die(1, "/dev/mediakeys opened \"r\"", r);
    int64_t a64 = os64_open("/dev/mediakeys", "u");
    if (a64 < 0)
        return die(1, "/dev/mediakeys would not open \"u\"", a64);
    int32_t a = (int32_t)a64;
    char small[4];
    if ((r = os64_read_for(a, small, sizeof(small), 0)) != -1)
        return die(1, "a read shorter than a record was not refused", r);
    if ((r = os64_write(a, "Playing", 7)) >= 0 || (r = os64_write(a, "play", 4)) >= 0 ||
        (r = os64_write(a, " paused", 7)) >= 0)
        return die(1, "a word that is no state was taken", r);
    if (!say(a, "stopped\n"))
        return die(1, "a state with a newline was refused", 0);
    char path[64];
    os64_snprintf(path, sizeof(path), "/proc/%lu/handles", (unsigned long)os64_taskid());
    int64_t ph = os64_open(path, "r");
    int64_t pn = ph < 0 ? ph : os64_read((int32_t)ph, gReport, sizeof(gReport) - 1);
    if (ph >= 0) os64_close((int32_t)ph);
    gReport[pn > 0 ? pn : 0] = 0;
    if (pn <= 0 || find(gReport, "mediakeys") == NULL)
        return die(1, "the session is not listed as mediakeys in /proc/<id>/handles", pn);

    // 2. The rule.
    int64_t b64 = os64_open("/dev/mediakeys", "u");
    if (b64 < 0)
        return die(2, "a second session would not open", b64);
    int32_t b = (int32_t)b64;
    if (!say(b, "playing") || !press("next"))
        return die(2, "could not set up", 0);
    os64_media_key_t k;
    int64_t before_us = os64_micros();
    if ((r = take(b, 0, &k)) != OS64_MEDIA_KEY_NEXT || !nothing_waiting(a))
        return die(2, "the only session playing did not take Next", r);
    if (k.serial != 1 || (int64_t)k.at_us < before_us - 1000000 || (int64_t)k.at_us > os64_micros())
        return die(2, "the record's serial or time is wrong", k.serial);
    if (!report_has("last_rule: playing"))
        return die(2, "the report does not name the playing rule", 0);
    if (!say(a, "playing") || !press("next") || (r = take(a, 0, NULL)) != OS64_MEDIA_KEY_NEXT || !nothing_waiting(b))
        return die(2, "the newest to say playing did not take Next", r);
    if (!say(b, "playing") || !press("next") || (r = take(a, 0, NULL)) != OS64_MEDIA_KEY_NEXT)
        return die(2, "saying playing again moved the keys", r);
    if (!press("play") || (r = take(a, 0, NULL)) != OS64_MEDIA_KEY_PAUSE)
        return die(2, "Play to a playing session was not given as Pause", r);
    if (!say(a, "paused") || !say(b, "paused") || !press("pause") || (r = take(b, 0, NULL)) != OS64_MEDIA_KEY_PLAY ||
        !nothing_waiting(a))
        return die(2, "Pause with none playing was not given to the most recently active as Play", r);
    if (!say(a, "stopped") || !press("previous") || (r = take(a, 0, NULL)) != OS64_MEDIA_KEY_PREVIOUS)
        return die(2, "Previous did not go to the most recently active", r);
    static const char kMixed[] = "press next\npress bogus\npress stop\n";
    int64_t mh = os64_open("/sys/mediakeys", "w");
    r = mh < 0 ? mh : os64_write((int32_t)mh, kMixed, sizeof(kMixed) - 1);
    if (mh >= 0) os64_close((int32_t)mh);
    if (r != -1)
        return die(2, "a write with a refused line was not answered refused", r);
    if ((r = take(a, 0, NULL)) != OS64_MEDIA_KEY_NEXT || (r = take(a, 0, NULL)) != OS64_MEDIA_KEY_STOP)
        return die(2, "a refused line stopped the lines around it", r);

    // 3. A full ring is passed over.
    if (!say(a, "playing"))
        return die(3, "could not set up", 0);
    for (int i = 0; i < RING; i++)
        if (!press("next"))
            return die(3, "a press was refused", i);
    if (!press("stop") || (r = take(b, 0, NULL)) != OS64_MEDIA_KEY_STOP)
        return die(3, "a press was not passed over a full ring to the other session", r);
    if (!report_has("passed-over-full"))
        return die(3, "the report does not show the pass", 0);
    uint32_t serial = 0;
    for (int i = 0; i < RING; i++) {
        if ((r = take(a, 0, &k)) != OS64_MEDIA_KEY_NEXT || (i > 0 && k.serial != serial + 1))
            return die(3, "the full ring's presses are not all there, in order", r);
        serial = k.serial;
    }
    if (!nothing_waiting(a))
        return die(3, "more than eight in the ring", 0);

    // 4. Parked, refused, woken.
    int32_t held = a;
    int64_t reader = os64_thread(read_forever, &held);
    if (reader < 0)
        return die(4, "no reader thread", reader);
    os64_sleep(SETTLE_MS);
    if ((r = os64_read_for(a, &k, sizeof(k), 0)) != OS64_MEDIA_KEYS_BUSY)
        return die(4, "a second reader was not refused BUSY while one is parked", r);
    int64_t before = os64_micros();
    if (!press("next"))
        return die(4, "the press was refused", 0);
    int64_t answer = -99;
    os64_thread_join((int32_t)reader, &answer);
    int64_t woke_us = os64_micros() - before;
    os64_close((int32_t)reader);
    if (answer != OS64_MEDIA_KEY_NEXT || woke_us > PROMPT_US)
        return die(4, "a press did not wake the parked reader, promptly", answer);

    // 5. A handled signal ends a parked read.
    if (os64_signal_set_handler(OS64_SIGINT, on_interrupt) < 0)
        return die(5, "the SIGINT handler was refused", 0);
    char me[24];
    os64_snprintf(me, sizeof(me), "%lu", (unsigned long)os64_taskid());
    int64_t kid = os64_spawn("/tests/mediakeystest", (char *[]){ "/tests/mediakeystest", "poke", me, 0 });
    if (kid < 0)
        return die(5, "could not spawn the sender", kid);
    r = os64_read_for(a, &k, sizeof(k), 10000);
    if (r != OS64_INTERRUPTED || gHandled != OS64_SIGINT)
        return die(5, "a handled signal did not end the parked read with INTERRUPTED", r);
    int32_t code = -1;
    while (os64_wait(kid, &code) == OS64_INTERRUPTED)
        ;
    os64_signal_set_handler(OS64_SIGINT, OS64_SIG_DEFAULT);
    if (code != 0)
        return die(5, "the sender could not write our ctl", code);

    // 6. Closed under a parked read.
    held = b;
    reader = os64_thread(read_forever, &held);
    if (reader < 0)
        return die(6, "no reader thread", reader);
    os64_sleep(SETTLE_MS);
    before = os64_micros();
    os64_close(b);
    answer = -99;
    os64_thread_join((int32_t)reader, &answer);
    woke_us = os64_micros() - before;
    os64_close((int32_t)reader);
    if (answer != 0 || woke_us > PROMPT_US)
        return die(6, "a close under a parked reader did not end its read with 0, promptly", answer);
    if (!say(a, "stopped") || !press("next") || (r = take(a, 0, NULL)) != OS64_MEDIA_KEY_NEXT)
        return die(6, "a press reached a closed session", r);
    if (report_number("sessions: ") != 1)
        return die(6, "the closed session is still listed", report_number("sessions: "));

    // 7. No session: the kernel's, with nothing to pause.
    os64_close(a);
    int64_t unanswered = report_number("unanswered: ");
    if (!press("play"))
        return die(7, "the press was refused", 0);
    if (!report_has("last_rule: kernel") || !report_has("kernel: last refused") ||
        report_number("unanswered: ") != unanswered + 1)
        return die(7, "with no session and nothing playing, Play was not the kernel's, refused", unanswered);

    // 8. A killed program's session ends with it.
    kid = os64_spawn("/tests/mediakeystest", (char *[]){ "/tests/mediakeystest", "hold", 0 });
    if (kid < 0)
        return die(8, "could not spawn the holder", kid);
    for (int i = 0; i < 50 && report_number("sessions: ") != 1; i++)
        os64_sleep(20);
    if (report_number("sessions: ") != 1 || !report_has("state playing"))
        return die(8, "the holder's session did not appear", report_number("sessions: "));
    os64_snprintf(path, sizeof(path), "/proc/%lu/ctl", (unsigned long)kid);
    int64_t ctl = os64_open(path, "w");
    if (ctl < 0 || os64_write((int32_t)ctl, "kill", 4) != 4)
        return die(8, "could not kill the holder", ctl);
    os64_close((int32_t)ctl);
    os64_wait(kid, &code);
    if (report_number("sessions: ") != 0)
        return die(8, "the killed holder's session is still listed", report_number("sessions: "));

    // 9. The wall.
    int32_t all[SESSIONS];
    for (int i = 0; i < SESSIONS; i++) {
        int64_t s = os64_open("/dev/mediakeys", "u");
        if (s < 0)
            return die(9, "a session under the wall was refused", i);
        all[i] = (int32_t)s;
    }
    r = os64_open("/dev/mediakeys", "u");
    for (int i = 0; i < SESSIONS; i++)
        os64_close(all[i]);
    if (r != OS64_MEDIA_KEYS_FULL)
        return die(9, "the session past the wall was not refused FULL", r);

    os64_printf("mediakeystest: ok\n");
    return MKTEST_OK;
}

int main(int argc, char **argv)
{
    if (argc == 3 && os64_streq(argv[1], "poke"))
        return poke_main(argv[2]);
    if (argc == 2 && os64_streq(argv[1], "hold"))
        return hold_main();
    return run();
}
