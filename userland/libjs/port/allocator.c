#include "allocator.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "platform.h"

union JSPortBlock {
    max_align_t alignment;
    struct {
        JSPortBlock *previous, *next;
        JSPortAllocator *owner;
    } links;
};

static size_t header_size(const JSMallocState *state)
{
    const JSPortAllocator *owner = state->opaque;
    return owner != NULL && owner->tracked ? sizeof(JSPortBlock) : 0;
}

static void *base(const JSMallocState *state, void *ptr)
{
    return (unsigned char *)ptr - header_size(state);
}

static void link_block(JSPortAllocator *owner, JSPortBlock *block)
{
    block->links.owner = owner;
    block->links.previous = NULL;
    block->links.next = owner->blocks;
    if (owner->blocks != NULL) owner->blocks->links.previous = block;
    owner->blocks = block;
    owner->live_blocks++;
    owner->live_bytes += os64_malloc_size(block);
}

static void unlink_block(JSPortAllocator *owner, JSPortBlock *block)
{
    size_t bytes = os64_malloc_size(block);
    if (block->links.owner != owner || owner->live_blocks == 0 || bytes > owner->live_bytes)
        jsport_fatal("allocator ledger", __FILE__, __LINE__);
    if (block->links.previous != NULL) block->links.previous->links.next = block->links.next;
    else owner->blocks = block->links.next;
    if (block->links.next != NULL) block->links.next->links.previous = block->links.previous;
    owner->live_blocks--;
    owner->live_bytes -= bytes;
}

void jsport_allocator_reclaim(JSPortAllocator *owner)
{
    while (owner->blocks != NULL) {
        JSPortBlock *block = owner->blocks;
        unlink_block(owner, block);
        os64_free(block);
    }
    if (owner->live_blocks != 0 || owner->live_bytes != 0)
        jsport_fatal("allocator ledger", __FILE__, __LINE__);
}

static size_t usable(const void *ptr)
{
    return ptr != NULL ? os64_malloc_size(ptr) : 0;
}

static size_t limit(const JSMallocState *state)
{
    const JSPortAllocator *owner = state->opaque;
    return owner != NULL && owner->allocation_limit < state->malloc_limit
        ? owner->allocation_limit : state->malloc_limit;
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
    size_t header = header_size(state);
    if (size > SIZE_MAX - header) {
        refused(state, JSPORT_ALLOC_LIMIT);
        return NULL;
    }
    size_t ceiling = limit(state);
    if (state->malloc_size > ceiling || size + header > ceiling - state->malloc_size) {
        refused(state, JSPORT_ALLOC_LIMIT);
        return NULL;
    }
    void *ptr = os64_malloc(size + header);
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
    if (header != 0) link_block(state->opaque, ptr);
    return (unsigned char *)ptr + header;
}

static void release(JSMallocState *state, void *ptr)
{
    if (ptr == NULL)
        return;
    void *block = base(state, ptr);
    size_t charged = usable(block);
    if (state->malloc_count == 0 || charged > state->malloc_size)
        jsport_fatal("allocator accounting", __FILE__, __LINE__);
    state->malloc_size -= charged;
    state->malloc_count--;
    if (header_size(state) != 0) unlink_block(state->opaque, block);
    os64_free(block);
}

static void *resize(JSMallocState *state, void *ptr, size_t size)
{
    if (ptr == NULL)
        return size == 0 ? NULL : allocate(state, size);
    if (size == 0) {
        release(state, ptr);
        return NULL;
    }
    size_t header = header_size(state);
    void *old_block = base(state, ptr);
    size_t old = usable(old_block);
    if (old > state->malloc_size)
        jsport_fatal("allocator accounting", __FILE__, __LINE__);
    size_t retained = state->malloc_size - old;
    size_t ceiling = limit(state);
    if (size > SIZE_MAX - header || retained > ceiling || size + header > ceiling - retained) {
        refused(state, JSPORT_ALLOC_LIMIT);
        return NULL;
    }
    /* The allocator can round a request above the remaining budget. A separate
     * block lets that failure leave the original bytes and accounting intact.
     * During copying both blocks exist; the limit counts retained allocator charge. */
    void *next = os64_malloc(size + header);
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
    size_t payload = old - header;
    os64_memcpy((unsigned char *)next + header, ptr, payload < size ? payload : size);
    if (header != 0) {
        unlink_block(state->opaque, old_block);
        link_block(state->opaque, next);
    }
    os64_free(old_block);
    state->malloc_size = retained + charged;
    return (unsigned char *)next + header;
}

const JSMallocFunctions jsport_malloc_functions = {allocate, release, resize, usable};

static size_t tracked_usable(const void *ptr)
{
    return ptr != NULL ? os64_malloc_size((const JSPortBlock *)ptr - 1) - sizeof(JSPortBlock) : 0;
}

const JSMallocFunctions jsport_tracked_malloc_functions = {
    allocate, release, resize, tracked_usable
};
