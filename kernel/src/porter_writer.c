#include "porter_writer.h"
#include "random.h"
#include "spinlock.h"

static spinlock_t writer_lock;
static const struct s_thread *writer;
static os64_porter_owner_t identity;

int porter_writer_acquire(struct s_thread *thread, uint64_t task, os64_porter_owner_t *out)
{
    uint64_t flags = spinlock_acquire_irqsave(&writer_lock);
    int status = OS64_PORTER_BUSY;
    if (!writer) {
        if (!identity.acquisition) {
            if (!random_seeded()) {
                spinlock_release_irqrestore(&writer_lock, flags);
                return OS64_PORTER_UNSEEDED;
            }
            random_bytes(identity.boot, sizeof(identity.boot));
        }
        if (identity.acquisition == UINT64_MAX) {
            spinlock_release_irqrestore(&writer_lock, flags);
            return -1;
        }
        writer = thread;
        identity.acquisition++;
        identity.task = task;
        *out = identity;
        status = 0;
    }
    spinlock_release_irqrestore(&writer_lock, flags);
    return status;
}

bool porter_writer_held(const struct s_thread *thread)
{
    uint64_t flags = spinlock_acquire_irqsave(&writer_lock);
    bool held = thread && writer == thread;
    spinlock_release_irqrestore(&writer_lock, flags);
    return held;
}

// Release at the owning thread's exit boundary. The thread pointer lasts
// through this point, including any sleeping storage syscall before exit.
void porter_writer_depart(const struct s_thread *thread)
{
    uint64_t flags = spinlock_acquire_irqsave(&writer_lock);
    if (writer == thread)
        writer = 0;
    spinlock_release_irqrestore(&writer_lock, flags);
}
