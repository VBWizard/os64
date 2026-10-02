#include "allocator.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "platform.h"

static size_t usable(const void *ptr)
{
    return ptr != NULL ? os64_malloc_size(ptr) : 0;
}

static size_t limit(const JSMallocState *state)
{
    const JSPortAllocator *owner = state->opaque;
    return owner != NULL && owner->payload_limit < state->malloc_limit
        ? owner->payload_limit : state->malloc_limit;
}

static void refused(JSMallocState *state, unsigned reason)
{
    JSPortAllocator *owner = state->opaque;
    if (owner != NULL) owner->failures |= reason;
}

static void *allocate(JSMallocState *state, size_t size)
{
    if (size == 0)
        jsport_fatal("zero-byte allocation", __FILE__, __LINE__);
    size_t ceiling = limit(state);
    if (state->malloc_size > ceiling || size > ceiling - state->malloc_size) {
        refused(state, JSPORT_ALLOC_LIMIT);
        return NULL;
    }
    void *ptr = os64_malloc(size);
    if (ptr == NULL) {
        refused(state, JSPORT_ALLOC_OOM);
        return NULL;
    }
    size_t charged = usable(ptr);
    if (charged > ceiling - state->malloc_size) {
        refused(state, JSPORT_ALLOC_LIMIT);
        os64_free(ptr);
        return NULL;
    }
    state->malloc_size += charged;
    state->malloc_count++;
    return ptr;
}

static void release(JSMallocState *state, void *ptr)
{
    if (ptr == NULL)
        return;
    size_t charged = usable(ptr);
    if (state->malloc_count == 0 || charged > state->malloc_size)
        jsport_fatal("allocator accounting", __FILE__, __LINE__);
    state->malloc_size -= charged;
    state->malloc_count--;
    os64_free(ptr);
}

static void *resize(JSMallocState *state, void *ptr, size_t size)
{
    if (ptr == NULL)
        return size == 0 ? NULL : allocate(state, size);
    if (size == 0) {
        release(state, ptr);
        return NULL;
    }
    size_t old = usable(ptr);
    if (old > state->malloc_size)
        jsport_fatal("allocator accounting", __FILE__, __LINE__);
    size_t retained = state->malloc_size - old;
    size_t ceiling = limit(state);
    if (retained > ceiling || size > ceiling - retained) {
        refused(state, JSPORT_ALLOC_LIMIT);
        return NULL;
    }
    /* The allocator can round a request above the remaining budget. A separate
     * block lets that failure leave the original bytes and accounting intact.
     * During copying both blocks exist; the limit counts retained payload. */
    void *next = os64_malloc(size);
    if (next == NULL) {
        refused(state, JSPORT_ALLOC_OOM);
        return NULL;
    }
    size_t charged = usable(next);
    if (charged > ceiling - retained) {
        refused(state, JSPORT_ALLOC_LIMIT);
        os64_free(next);
        return NULL;
    }
    os64_memcpy(next, ptr, old < size ? old : size);
    os64_free(ptr);
    state->malloc_size = retained + charged;
    return next;
}

const JSMallocFunctions jsport_malloc_functions = {allocate, release, resize, usable};
