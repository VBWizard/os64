#include "appearance.h"
#include "gui/gui_internal.h"
#include "gui/window.h"
#include "memory/memcpy.h"

// Bounded storage avoids allocation or reclamation while holding the GUI lock.
// An open file owns a copy, so a later publisher cannot change its snapshot.
static uint64_t s_generation;
static size_t s_length;
static char s_payload[OS64_APPEARANCE_PAYLOAD_MAX];

int appearance_snapshot(char *out, size_t cap)
{
    if (!out || cap < OS64_APPEARANCE_MAX) return -1;
    uint64_t flags = spinlock_acquire_irqsave(&kGuiLock);
    size_t head = os64_appearance_header_write(out, s_generation);
    memcpy(out + head, s_payload, s_length);
    int result = (int)(head + s_length);
    spinlock_release_irqrestore(&kGuiLock, flags);
    return result;
}

int appearance_publish(const char *bytes, size_t length)
{
    uint64_t expected;
    size_t body;
    if (length > OS64_APPEARANCE_MAX ||
        !os64_appearance_header_read(bytes, length, &expected, &body) ||
        length - body > OS64_APPEARANCE_PAYLOAD_MAX) return -1;
    uint64_t flags = spinlock_acquire_irqsave(&kGuiLock);
    if (expected != s_generation || s_generation == UINT64_MAX) {
        spinlock_release_irqrestore(&kGuiLock, flags);
        return -1;
    }
    s_length = length - body;
    memcpy(s_payload, bytes + body, s_length);
    ++s_generation;
    wm_appearance_changed(s_generation);
    spinlock_release_irqrestore(&kGuiLock, flags);
    return (int)length;
}

size_t appearance_length(void)
{
    char header[OS64_APPEARANCE_HEADER_MAX];
    uint64_t flags = spinlock_acquire_irqsave(&kGuiLock);
    size_t result = os64_appearance_header_write(header, s_generation) + s_length;
    spinlock_release_irqrestore(&kGuiLock, flags);
    return result;
}
