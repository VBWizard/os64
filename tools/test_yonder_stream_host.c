// test_yonder_stream_host.c — the navigation mailbox's stream on the host
// (docs/design/pending/DOM_D4.md): a worker thread posting a page's head,
// body and verdict through the bounded ring while a window thread takes
// them, over real pipes, under the sanitizers. Each case holds the ring to
// one sentence of mail.h's contract: in order and whole; a full ring makes
// the poster wait and the window's note wakes it; a stale answer is not
// room; cancellation ends a wait; the head and the verdict arrive once; the
// ring goes with the mailbox.

#define _DEFAULT_SOURCE

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mail.h"
#include "os64/io.h"
#include "os64/syscall_numbers.h"

// ── What libos64 would be ───────────────────────────────────────────────

// The console goes nowhere; a mailbox's pipe is a real one. The harness
// owns this call, so it can count what goes down the pipe: a note numbered
// 0 is the window saying it made room, and the cases below hold the rule
// that one is written only while a poster is waiting for that room.
static size_t s_room_notes;

int64_t os64_write(int32_t handle, const void *buf, size_t len)
{
    if (handle <= 2)
        return (int64_t)len;
    if (len == 8 && *(const uint32_t *)buf == 0)
        __atomic_add_fetch(&s_room_notes, 1, __ATOMIC_RELAXED);
    return (int64_t)write(handle, buf, len);
}

int64_t os64_pipe(int32_t h[2])
{
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    h[0] = fds[0];
    h[1] = fds[1];
    return 0;
}

int64_t os64_read_for(int32_t handle, void *buf, size_t len, uint64_t timeout_ms)
{
    struct pollfd p = {handle, POLLIN, 0};
    int got = poll(&p, 1, (int)timeout_ms);
    if (got == 0)
        return OS64_ERR_TIMEOUT;
    if (got < 0)
        return -1;
    return (int64_t)read(handle, buf, len);
}

int64_t os64_close(int32_t handle)
{
    return close(handle);
}

void os64_yield(void)
{
    sched_yield();
}

static size_t live;

void *os64_malloc(size_t size)
{
    void *p = malloc(size != 0 ? size : 1);
    if (p != NULL)
        __atomic_add_fetch(&live, 1, __ATOMIC_RELAXED);
    return p;
}

void *os64_calloc(size_t n, size_t size)
{
    void *p = calloc(n != 0 ? n : 1, size != 0 ? size : 1);
    if (p != NULL)
        __atomic_add_fetch(&live, 1, __ATOMIC_RELAXED);
    return p;
}

void os64_free(void *p)
{
    if (p != NULL)
        __atomic_sub_fetch(&live, 1, __ATOMIC_RELAXED);
    free(p);
}

// ── The harness ─────────────────────────────────────────────────────────

static int checks, failures;

static void expect(const char *name, bool ok, const char *detail)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL %s%s%s\n", name, detail ? ": " : "", detail ? detail : "");
    }
}

static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static bool never(void *ctx)
{
    (void)ctx;
    return false;
}

// Cancelled once a counter the test owns says so.
static int s_cancel_after;
static bool after(void *ctx)
{
    int *calls = ctx;
    return ++*calls > s_cancel_after;
}

static int open_fds(void)
{
    int n = 0;
    for (int fd = 0; fd < 1024; fd++)
        n += fcntl(fd, F_GETFD) != -1;
    return n;
}

// The body a worker posts: `count` chunks whose byte i of chunk k is a
// function of (k, i), so the taker can check every byte without keeping a
// copy. Chunk lengths vary, a few of them a full slot and one of them
// larger than a slot, which the post splits.
static size_t chunk_len(size_t k)
{
    static const size_t lens[] = {1, 17, 4095, 16384, 16384, 9000, 16385, 300, 16384, 2};
    return lens[k % (sizeof(lens) / sizeof(lens[0]))];
}

static uint8_t byte_at(size_t k, size_t i)
{
    return (uint8_t)(k * 31 + i * 7 + 3);
}

typedef struct {
    yonder_mail_t *mail;
    size_t count;
    uint64_t took_ms;           // how long the posting took
    bool all_posted;            // every chunk went in
    bool done;                  // the poster has returned, however it went
    bool (*cancelled)(void *ctx);
    void *ctx;
} Poster;

static void *post_body(void *arg)
{
    Poster *p = arg;
    uint8_t buf[32768];
    uint64_t t0 = now_ms();
    p->all_posted = true;
    for (size_t k = 0; k < p->count; k++) {
        size_t len = chunk_len(k);
        for (size_t i = 0; i < len; i++)
            buf[i] = byte_at(k, i);
        if (!yonder_mail_post(p->mail, buf, len, p->cancelled, p->ctx)) {
            p->all_posted = false;
            break;
        }
    }
    p->took_ms = now_ms() - t0;
    __atomic_store_n(&p->done, true, __ATOMIC_RELEASE);
    return NULL;
}

// Takes everything `count` chunks amount to, checking every byte and the
// order, pausing `pause_us` between takes. Answers how many bytes came.
static size_t take_body(yonder_mail_t *mail, size_t count, unsigned pause_us, bool *in_order)
{
    size_t k = 0, i = 0, total = 0;
    *in_order = true;
    uint8_t buf[YONDER_STREAM_CHUNK];
    uint64_t deadline = now_ms() + 20000;
    while (k < count) {
        size_t n = yonder_mail_take(mail, buf, sizeof(buf));
        if (n == 0) {
            if (now_ms() > deadline)
                break;
            usleep(200);
            continue;
        }
        for (size_t b = 0; b < n; b++) {
            while (k < count && i == chunk_len(k)) {
                k++;
                i = 0;
            }
            if (k == count || buf[b] != byte_at(k, i))
                *in_order = false;
            i++;
        }
        while (k < count && i == chunk_len(k)) {
            k++;
            i = 0;
        }
        total += n;
        if (pause_us != 0)
            usleep(pause_us);
    }
    return total;
}

static void whole_and_in_order(void)
{
    // A thousand chunks of every shape through sixteen slots, the taker
    // pausing now and then so the ring fills and drains many times over.
    yonder_mail_t *m = yonder_mail_new(1);
    way_head_t head;
    memset(&head, 0, sizeof(head));
    head.status = 200;
    snprintf(head.reason, sizeof(head.reason), "OK");
    snprintf(head.url, sizeof(head.url), "http://h/p");
    yonder_mail_post_head(m, &head);
    Poster p = {m, 1000, 0, false, false, never, NULL};
    pthread_t t;
    pthread_create(&t, NULL, post_body, &p);
    way_head_t got;
    expect("stream: the head comes first and once",
           yonder_mail_take_head(m, &got) && got.status == 200 && strcmp(got.url, "http://h/p") == 0 &&
               !yonder_mail_take_head(m, &got), NULL);
    size_t want = 0;
    for (size_t k = 0; k < 1000; k++)
        want += chunk_len(k);
    bool in_order;
    size_t total = take_body(m, 1000, 50, &in_order);
    pthread_join(t, NULL);
    char detail[96];
    snprintf(detail, sizeof(detail), "%zu of %zu bytes", total, want);
    expect("stream: every byte arrives, whole and in order", p.all_posted && total == want && in_order,
           detail);
    yonder_verdict_t v;
    memset(&v, 0, sizeof(v));
    v.page = true;
    v.fetch = OS64_FETCH_CUT;
    snprintf(v.reason, sizeof(v.reason), "the server hung up early");
    expect("stream: nothing waits before the verdict", !yonder_mail_streaming(m), NULL);
    yonder_mail_post_verdict(m, &v);
    yonder_verdict_t w;
    expect("stream: the verdict comes once",
           yonder_mail_streaming(m) && yonder_mail_take_verdict(m, &w) && w.page &&
               w.fetch == OS64_FETCH_CUT && strcmp(w.reason, v.reason) == 0 &&
               !yonder_mail_take_verdict(m, &w) && !yonder_mail_streaming(m), NULL);
    yonder_mail_drop(m);
}

static void full_ring_waits_and_is_woken(void)
{
    // Thirty-two slot-sized chunks into sixteen slots with nobody taking:
    // the poster must still be at it after a while. Then a window that
    // takes one every millisecond: the note down the pipe wakes the poster
    // at once, so the whole body goes in far less than the 100 ms steps it
    // would take if each wait ran its course (sixteen of them: 1.6 s).
    yonder_mail_t *m = yonder_mail_new(2);
    Poster p = {m, 32, 0, false, false, never, NULL};
    pthread_t t;
    pthread_create(&t, NULL, post_body, &p);
    usleep(150 * 1000);
    expect("stream: a full ring holds the poster", !__atomic_load_n(&p.done, __ATOMIC_ACQUIRE), NULL);
    // A stale answer in the pipe is not room: the poster reads it and
    // waits on.
    yonder_mail_answer(m, 7, true);
    usleep(150 * 1000);
    expect("stream: a stale answer is not room", !__atomic_load_n(&p.done, __ATOMIC_ACQUIRE), NULL);
    size_t notes_before = s_room_notes;
    uint64_t t0 = now_ms();
    bool in_order;
    size_t total = take_body(m, 32, 1000, &in_order);
    pthread_join(t, NULL);
    uint64_t took = now_ms() - t0;
    expect("stream: the window wrote room notes for a waiting poster", s_room_notes > notes_before, NULL);
    size_t want = 0;
    for (size_t k = 0; k < 32; k++)
        want += chunk_len(k);
    char detail[96];
    snprintf(detail, sizeof(detail), "%zu bytes in %lu ms", total, (unsigned long)took);
    expect("stream: room wakes the poster without waiting out the step",
           p.all_posted && total == want && in_order && took < 800, detail);
    yonder_mail_drop(m);
}

static void cancel_ends_a_wait(void)
{
    yonder_mail_t *m = yonder_mail_new(3);
    int calls = 0;
    // The ring fills on the first sixteen or so asks (a chunk larger than
    // a slot asks once per slot); the asks after that each cost a 100 ms
    // step of waiting, so the whole thing ends in well under a second.
    s_cancel_after = 20;
    Poster p = {m, 64, 0, false, false, after, &calls};
    pthread_t t;
    pthread_create(&t, NULL, post_body, &p);
    pthread_join(t, NULL);
    expect("stream: cancellation ends a post waiting for room", !p.all_posted && p.took_ms < 1500, NULL);
    // What was posted before the cancel is still there to take, and no
    // more than fits.
    uint8_t buf[YONDER_STREAM_CHUNK];
    int taken = 0;
    while (yonder_mail_take(m, buf, sizeof(buf)) != 0)
        taken++;
    expect("stream: the ring holds what was posted, up to its slots", taken == YONDER_STREAM_CHUNKS, NULL);
    yonder_mail_drop(m);
}

static void notes_keep_apart(void)
{
    // A room note down the pipe is not an answer: a worker waiting on
    // question 4 reads past it to the real answer.
    yonder_mail_t *m = yonder_mail_new(4);
    uint8_t buf[YONDER_STREAM_CHUNK];
    memset(buf, 1, sizeof(buf));
    // Fill the ring and have the window take while the poster is NOT
    // waiting: no note is written, since nobody would read it.
    for (int i = 0; i < YONDER_STREAM_CHUNKS; i++)
        yonder_mail_post(m, buf, 100, never, NULL);
    size_t notes_before = s_room_notes;
    expect("stream: a ring of sixteen takes no more", yonder_mail_take(m, buf, sizeof(buf)) == 100, NULL);
    expect("stream: no note for a take with nobody waiting", s_room_notes == notes_before, NULL);
    // Nothing is in the pipe to read: a wait that gives up on its second
    // ask spends a whole 100 ms step finding that out. Had the take left a
    // note, the wait would have read it and ended at once.
    int calls = 0;
    s_cancel_after = 1;
    uint64_t t0 = now_ms();
    bool answered = yonder_mail_wait(m, 9, after, &calls);
    expect("stream: a take with nobody waiting writes no note", !answered && now_ms() - t0 >= 90, NULL);
    uint32_t number = yonder_mail_ask(m, "go?");
    yonder_mail_answer(m, number, true);
    expect("stream: the answer is read as the answer", yonder_mail_wait(m, number, never, NULL) == true,
           NULL);
    // A small cap takes nothing and loses nothing.
    expect("stream: a buffer smaller than a chunk takes nothing", yonder_mail_take(m, buf, 10) == 0, NULL);
    int taken = 0;
    while (yonder_mail_take(m, buf, sizeof(buf)) != 0)
        taken++;
    expect("stream: the rest is still there", taken == YONDER_STREAM_CHUNKS - 1, NULL);
    yonder_mail_drop(m);
}

static void gone_with_the_mailbox(void)
{
    size_t before = live;
    int fds = open_fds();
    yonder_mail_t *m = yonder_mail_new(5);
    yonder_mail_hold(m);
    uint8_t buf[100];
    memset(buf, 2, sizeof(buf));
    yonder_mail_post(m, buf, sizeof(buf), never, NULL);
    yonder_mail_drop(m);
    expect("stream: the ring stays while a holder remains",
           yonder_mail_streaming(m) && open_fds() == fds + 2, NULL);
    yonder_mail_drop(m);
    expect("stream: the ring and the pipe go with the last holder", live == before && open_fds() == fds,
           NULL);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    whole_and_in_order();
    full_ring_waits_and_is_woken();
    cancel_ends_a_wait();
    notes_keep_apart();
    gone_with_the_mailbox();
    expect("nothing leaked", live == 0, NULL);
    printf("yonder stream: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
