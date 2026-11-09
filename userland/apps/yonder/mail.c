// mail.c — a navigation's mailbox (mail.h).

#include "mail.h"
#include "os64/io.h"
#include "os64/lock.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "os64/syscall_numbers.h"

#define MAIL_TEXT 512

// A note down the pipe: an answer to question `number`, or, numbered 0 —
// which no question is — word from the window that it took from a full
// ring. One pipe carries both because a task has 64 handles and this is
// the browser; the two waits each ignore the other's notes.
typedef struct {
    uint32_t number;
    uint32_t yes;
} Note;

struct yonder_mail {
    uint32_t refs;              // atomic
    uint64_t generation;
    os64_lock_t lock;
    char progress[MAIL_TEXT];
    bool progress_new;
    uint32_t asked;             // the newest question's number
    char question[MAIL_TEXT];
    bool question_new;
    int32_t pipe[2];            // [0] the job reads notes, [1] the window writes them
    // The stream. The ring's slots trail the struct (one allocation).
    way_head_t head;
    bool head_new;
    yonder_verdict_t verdict;
    bool verdict_new;
    uint32_t first, count;      // the oldest slot waiting, and how many wait
    uint32_t lens[YONDER_STREAM_CHUNKS];
    // The worker is asleep on the pipe for room. Set under the lock before
    // it sleeps, cleared when it wakes, so the window writes a note only
    // when one will be read: a note per chunk would pile up unread in the
    // pipe while the two sides ran in lockstep, until a write blocked the
    // window.
    bool waiting;
    uint8_t ring[];
};

static uint8_t *slot(yonder_mail_t *m, uint32_t i)
{
    return m->ring + (size_t)i * YONDER_STREAM_CHUNK;
}

yonder_mail_t *yonder_mail_new(uint64_t generation)
{
    yonder_mail_t *m = os64_calloc(1, sizeof(*m) + (size_t)YONDER_STREAM_CHUNKS * YONDER_STREAM_CHUNK);
    if (m == NULL)
        return NULL;
    if (os64_pipe(m->pipe) < 0) {
        os64_free(m);
        return NULL;
    }
    m->refs = 1;
    m->generation = generation;
    return m;
}

void yonder_mail_hold(yonder_mail_t *m)
{
    __atomic_add_fetch(&m->refs, 1, __ATOMIC_RELAXED);
}

void yonder_mail_drop(yonder_mail_t *m)
{
    if (m == NULL || __atomic_sub_fetch(&m->refs, 1, __ATOMIC_ACQ_REL) != 0)
        return;
    os64_close(m->pipe[0]);
    os64_close(m->pipe[1]);
    os64_free(m);
}

uint64_t yonder_mail_generation(const yonder_mail_t *m)
{
    return m->generation;
}

void yonder_mail_progress(yonder_mail_t *m, const char *sentence)
{
    os64_lock_acquire(&m->lock);
    os64_strcopy(m->progress, sizeof(m->progress), sentence);
    m->progress_new = true;
    os64_lock_release(&m->lock);
}

uint32_t yonder_mail_ask(yonder_mail_t *m, const char *question)
{
    os64_lock_acquire(&m->lock);
    uint32_t number = ++m->asked;
    os64_strcopy(m->question, sizeof(m->question), question);
    m->question_new = true;
    os64_lock_release(&m->lock);
    return number;
}

// One note off the pipe, or none within 100 ms. False when the pipe broke:
// nobody is left on the other side.
static bool note_read(yonder_mail_t *m, Note *note, bool *got_one)
{
    size_t got = 0;
    *got_one = false;
    for (;;) {
        int64_t n = os64_read_for(m->pipe[0], (char *)note + got, sizeof(*note) - got, 100);
        if (n == OS64_ERR_TIMEOUT)
            return true;            // a partial note, if any, is finished by the next read
        if (n <= 0)
            return false;
        got += (size_t)n;
        if (got == sizeof(*note)) {
            *got_one = true;
            return true;
        }
    }
}

bool yonder_mail_wait(yonder_mail_t *m, uint32_t number, bool (*cancelled)(void *ctx), void *ctx)
{
    Note note;
    size_t got = 0;
    for (;;) {
        if (cancelled(ctx))
            return false;
        int64_t n = os64_read_for(m->pipe[0], (char *)&note + got, sizeof(note) - got, 100);
        if (n == OS64_ERR_TIMEOUT)
            continue;
        if (n <= 0)
            return false;           // the pipe broke: nobody is left to answer
        got += (size_t)n;
        if (got < sizeof(note))
            continue;
        got = 0;
        if (note.number == number)
            return note.yes != 0;
    }
}

static void note_write(yonder_mail_t *m, uint32_t number, bool yes)
{
    Note note = {number, yes ? 1u : 0u};
    (void)os64_write(m->pipe[1], &note, sizeof(note));
}

bool yonder_mail_take_progress(yonder_mail_t *m, char *out, size_t cap)
{
    os64_lock_acquire(&m->lock);
    bool fresh = m->progress_new;
    if (fresh)
        os64_strcopy(out, cap, m->progress);
    m->progress_new = false;
    os64_lock_release(&m->lock);
    return fresh;
}

bool yonder_mail_take_question(yonder_mail_t *m, uint32_t *number, char *out, size_t cap)
{
    os64_lock_acquire(&m->lock);
    bool fresh = m->question_new;
    if (fresh) {
        os64_strcopy(out, cap, m->question);
        *number = m->asked;
    }
    m->question_new = false;
    os64_lock_release(&m->lock);
    return fresh;
}

void yonder_mail_answer(yonder_mail_t *m, uint32_t number, bool yes)
{
    note_write(m, number, yes);
}

// ── The stream ──────────────────────────────────────────────────────────

void yonder_mail_post_head(yonder_mail_t *m, const way_head_t *head)
{
    os64_lock_acquire(&m->lock);
    m->head = *head;
    m->head_new = true;
    os64_lock_release(&m->lock);
}

bool yonder_mail_post(yonder_mail_t *m, const void *bytes, size_t len,
                      bool (*cancelled)(void *ctx), void *ctx)
{
    const uint8_t *at = bytes;
    while (len > 0) {
        if (cancelled(ctx))
            return false;
        os64_lock_acquire(&m->lock);
        if (m->count < YONDER_STREAM_CHUNKS) {
            uint32_t i = (m->first + m->count) % YONDER_STREAM_CHUNKS;
            size_t take = len < YONDER_STREAM_CHUNK ? len : YONDER_STREAM_CHUNK;
            os64_memcpy(slot(m, i), at, take);
            m->lens[i] = (uint32_t)take;
            m->count++;
            os64_lock_release(&m->lock);
            at += take;
            len -= take;
            continue;
        }
        m->waiting = true;
        os64_lock_release(&m->lock);
        Note note;
        bool got;
        bool alive = note_read(m, &note, &got);    // room, a stale answer, or nothing yet
        os64_lock_acquire(&m->lock);
        m->waiting = false;
        os64_lock_release(&m->lock);
        if (!alive)
            return false;
    }
    return true;
}

void yonder_mail_post_verdict(yonder_mail_t *m, const yonder_verdict_t *verdict)
{
    os64_lock_acquire(&m->lock);
    m->verdict = *verdict;
    m->verdict_new = true;
    os64_lock_release(&m->lock);
}

bool yonder_mail_take_head(yonder_mail_t *m, way_head_t *out)
{
    os64_lock_acquire(&m->lock);
    bool fresh = m->head_new;
    if (fresh)
        *out = m->head;
    m->head_new = false;
    os64_lock_release(&m->lock);
    return fresh;
}

size_t yonder_mail_take(yonder_mail_t *m, void *buf, size_t cap)
{
    if (cap < YONDER_STREAM_CHUNK)
        return 0;
    os64_lock_acquire(&m->lock);
    if (m->count == 0) {
        os64_lock_release(&m->lock);
        return 0;
    }
    size_t len = m->lens[m->first];
    os64_memcpy(buf, slot(m, m->first), len);
    m->first = (m->first + 1) % YONDER_STREAM_CHUNKS;
    m->count--;
    bool wake = m->waiting;
    os64_lock_release(&m->lock);
    if (wake)
        note_write(m, 0, false);
    return len;
}

bool yonder_mail_take_verdict(yonder_mail_t *m, yonder_verdict_t *out)
{
    os64_lock_acquire(&m->lock);
    bool fresh = m->verdict_new;
    if (fresh)
        *out = m->verdict;
    m->verdict_new = false;
    os64_lock_release(&m->lock);
    return fresh;
}

bool yonder_mail_streaming(yonder_mail_t *m)
{
    os64_lock_acquire(&m->lock);
    bool some = m->head_new || m->count > 0 || m->verdict_new;
    os64_lock_release(&m->lock);
    return some;
}
