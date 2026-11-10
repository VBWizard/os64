// host_bell.c — see host_bell.h.
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "host_bell.h"
#include "os64/os64.h"

#define HOST_BELLS 256
static struct {
    bool alive, rung, waiting, interrupted;
    pthread_cond_t wake;
} s_bells[HOST_BELLS];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned s_alive;
static unsigned long s_waits;
bool host_bell_never_blocks, host_bell_refuse_make;
void (*host_bell_slept)(uint64_t ms);

unsigned host_bell_alive(void)
{
    pthread_mutex_lock(&s_lock);
    unsigned n = s_alive;
    pthread_mutex_unlock(&s_lock);
    return n;
}

unsigned long host_bell_waits(void)
{
    pthread_mutex_lock(&s_lock);
    unsigned long n = s_waits;
    pthread_mutex_unlock(&s_lock);
    return n;
}

int64_t os64_bell_make(os64_bell_t *bell)
{
    bell->handle = -1;
    if (host_bell_refuse_make)
        return -1;
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < HOST_BELLS; i++)
        if (!s_bells[i].alive) {
            s_bells[i].alive = true;
            s_bells[i].rung = s_bells[i].waiting = s_bells[i].interrupted = false;
            pthread_cond_init(&s_bells[i].wake, NULL);
            s_alive++;
            bell->handle = i;
            break;
        }
    pthread_mutex_unlock(&s_lock);
    return bell->handle < 0 ? -1 : 0;
}

int64_t os64_bell_wait(os64_bell_t *bell, uint64_t timeout_ms)
{
    if (bell->handle < 0 || bell->handle >= HOST_BELLS)
        return -1;
    int64_t answer = 1;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    if (timeout_ms != OS64_WAIT_FOREVER) {
        until.tv_sec += (time_t)(timeout_ms / 1000u);
        until.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
        if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    }
    pthread_mutex_lock(&s_lock);
    __typeof__(&s_bells[0]) b = &s_bells[bell->handle];
    if (!b->alive || b->waiting)
        answer = -1;
    else if (!b->rung) {
        if (host_bell_never_blocks) {
            answer = OS64_ERR_TIMEOUT;
            if (host_bell_slept != NULL && timeout_ms != OS64_WAIT_FOREVER)
                host_bell_slept(timeout_ms);
        } else {
            b->waiting = true;
            b->interrupted = false;
            s_waits++;
            while (b->alive && !b->rung && !b->interrupted && answer == 1) {
                if (timeout_ms == OS64_WAIT_FOREVER)
                    pthread_cond_wait(&b->wake, &s_lock);
                else if (pthread_cond_timedwait(&b->wake, &s_lock, &until) == ETIMEDOUT && !b->rung)
                    answer = OS64_ERR_TIMEOUT;
            }
            b->waiting = false;
            if (!b->alive)
                answer = 0;                     // closed under the wait
            else if (answer == 1 && !b->rung)
                answer = OS64_INTERRUPTED;
            b->interrupted = false;
        }
    }
    if (answer == 1)
        b->rung = false;
    pthread_mutex_unlock(&s_lock);
    return answer;
}

int64_t os64_bell_ring(os64_bell_t *bell)
{
    pthread_mutex_lock(&s_lock);
    if (bell->handle < 0 || bell->handle >= HOST_BELLS || !s_bells[bell->handle].alive) {
        fprintf(stderr, "host_bell: a ring of a bell that is not one (handle %d): on os64 this byte "
                        "would be written to whatever took the number\n", bell->handle);
        abort();
    }
    s_bells[bell->handle].rung = true;
    pthread_cond_broadcast(&s_bells[bell->handle].wake);
    pthread_mutex_unlock(&s_lock);
    return 0;
}

void host_bell_interrupt(os64_bell_t *bell)
{
    pthread_mutex_lock(&s_lock);
    if (bell->handle >= 0 && bell->handle < HOST_BELLS && s_bells[bell->handle].alive) {
        s_bells[bell->handle].interrupted = true;
        pthread_cond_broadcast(&s_bells[bell->handle].wake);
    }
    pthread_mutex_unlock(&s_lock);
}

void os64_bell_close(os64_bell_t *bell)
{
    pthread_mutex_lock(&s_lock);
    if (bell->handle >= 0 && bell->handle < HOST_BELLS && s_bells[bell->handle].alive) {
        s_bells[bell->handle].alive = false;
        s_alive--;
        pthread_cond_broadcast(&s_bells[bell->handle].wake);
    }
    pthread_mutex_unlock(&s_lock);
    bell->handle = -1;
}
