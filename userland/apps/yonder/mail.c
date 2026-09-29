// mail.c — a navigation's mailbox (mail.h).

#include "mail.h"
#include "os64/io.h"
#include "os64/lock.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "os64/syscall_numbers.h"

#define MAIL_TEXT 512

// An answer on the pipe: which question, and the word.
typedef struct {
    uint32_t number;
    uint32_t yes;
} Answer;

struct yonder_mail {
    uint32_t refs;              // atomic
    uint64_t generation;
    os64_lock_t lock;
    char progress[MAIL_TEXT];
    bool progress_new;
    uint32_t asked;             // the newest question's number
    char question[MAIL_TEXT];
    bool question_new;
    int32_t pipe[2];            // [0] the job reads answers, [1] the window writes them
};

yonder_mail_t *yonder_mail_new(uint64_t generation)
{
    yonder_mail_t *m = os64_calloc(1, sizeof(*m));
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

bool yonder_mail_wait(yonder_mail_t *m, uint32_t number, bool (*cancelled)(void *ctx), void *ctx)
{
    Answer a;
    size_t got = 0;
    for (;;) {
        if (cancelled(ctx))
            return false;
        int64_t n = os64_read_for(m->pipe[0], (char *)&a + got, sizeof(a) - got, 100);
        if (n == OS64_ERR_TIMEOUT)
            continue;
        if (n <= 0)
            return false;           // the pipe broke: nobody is left to answer
        got += (size_t)n;
        if (got < sizeof(a))
            continue;
        got = 0;
        if (a.number == number)
            return a.yes != 0;
    }
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
    Answer a = {number, yes ? 1u : 0u};
    (void)os64_write(m->pipe[1], &a, sizeof(a));
}
